import 'dart:async';
import 'dart:typed_data';

import '../models/litchcraft_animation.dart';
import '../models/litchcraft_animation_codec.dart';

/// Additive schema-3 document transport. This intentionally has no capability
/// discovery or controller activation: deployed static Litchcraft remains 4c.
class LitchcraftV2Protocol {
  static const int magic = 0x4d;
  static const int version = 2;
  static const int maximumPacketBytes = 244;
  static const int minimumPacketBytes = 20;
  static const int maximumDocumentBytes = 12336;
  static const int schema = 3;

  static const int inspect = 1,
      timeline = 2,
      paint = 3,
      effect = 4,
      frameEdit = 5,
      undo = 6,
      redo = 7,
      snapshot = 8,
      replaceBeginOp = 9,
      replaceChunkOp = 10,
      replaceCommitOp = 11,
      cancelTransfer = 12,
      canvasEdit = 13,
      streamBegin = 14,
      streamChunk = 15,
      streamCommit = 16,
      streamCheckpoint = 17,
      frameStoreBegin = 18,
      frameStoreChunk = 19,
      frameStoreCheckpoint = 20,
      frameStoreCommit = 21,
      frameStoreCancel = 22,
      frameStoreSnapshot = 24,
      frameStoreResidents = 25,
      frameStoreSelect = 26;
  static const int state = 0x81,
      snapshotBegin = 0x84,
      snapshotChunk = 0x85,
      snapshotEnd = 0x86,
      changed = 0x87,
      timelineLow = 0x88,
      timelineHigh = 0x89,
      effectClockLow = 0x8b,
      effectClockHigh = 0x8c,
      frameStoreState = 0x8d,
      frameStoreCheckpointReceipt = 0x8f,
      frameStoreSnapshotChunk = 0x90,
      physicalStoreBatch = 0x91,
      frameStoreResidentsState = 0x92;

  static Uint8List request(
    int operation,
    int session,
    int request, [
    List<int> payload = const <int>[],
  ]) {
    if (operation < inspect ||
        operation > streamCheckpoint ||
        session < 0 ||
        session > 0xffffffff ||
        (session == 0 &&
            operation != inspect &&
            operation != snapshot &&
            operation != replaceBeginOp &&
            operation != replaceChunkOp &&
            operation != replaceCommitOp &&
            operation != streamBegin &&
            operation != streamChunk &&
            operation != streamCommit &&
            operation != streamCheckpoint &&
            operation != cancelTransfer) ||
        request < 1 ||
        request > 0xffff ||
        payload.length >
            (operation == replaceChunkOp || operation == streamChunk
                ? maximumPacketBytes - 9
                : 11) ||
        payload.any((int byte) => byte < 0 || byte > 0xff)) {
      throw ArgumentError('Invalid Litchcraft v2 request');
    }
    if (!_validPayload(operation, payload)) {
      throw ArgumentError('Invalid Litchcraft v2 operation payload');
    }
    final bytes = Uint8List(9 + payload.length);
    final data = ByteData.sublistView(bytes);
    bytes[0] = magic;
    bytes[1] = version;
    bytes[2] = operation;
    data.setUint32(3, session, Endian.little);
    data.setUint16(7, request, Endian.little);
    bytes.setRange(9, bytes.length, payload);
    return bytes;
  }

  static bool _validPayload(int operation, List<int> p) {
    int u16(int offset) => p[offset] | p[offset + 1] << 8;
    switch (operation) {
      case canvasEdit:
        // An empty payload is a read-only feature probe. Older v2 peers return
        // Unsupported, leaving the existing whole-document fallback available.
        return p.isEmpty ||
            (p.length == 1 && (p[0] == 2 || p[0] == 3)) ||
            (p.length == 10 &&
                p[4] < 8 &&
                p[5] > 0 &&
                p[5] <= 15 &&
                p[9] <= 8 &&
                (p[9] == 0 ||
                    (p[9] <= 4 &&
                        p[5] == 15 &&
                        p[6] == 0 &&
                        p[7] == 0 &&
                        p[8] == 0) ||
                    (p[9] >= 5 && p[9] <= 6) ||
                    (p[9] >= 7 && p[6] == 0 && p[7] == 0 && p[8] == 0)));
      case inspect:
        return p.isEmpty || (p.length == 1 && p[0] >= 1 && p[0] <= 7);
      case snapshot || undo || redo:
        return p.isEmpty;
      case timeline:
        return p.length == 2 && p[0] <= 2 && p[1] < 8;
      case paint || effect:
        if (p.length < 8 ||
            p.length > 11 ||
            p[0] >= 8 ||
            p[4] < 1 ||
            p[4] > 15 ||
            p.skip(8).any((square) => square >= 64)) {
          return false;
        }
        if (operation == paint) return p[3] & ~3 == 0;
        return p[1] <= 8 &&
            p[7] & ~3 == 0 &&
            (p[1] == 0
                ? u16(2) == 0
                : u16(2) >= 500 && u16(2) <= 10000 && u16(2) % 100 == 0);
      case frameEdit:
        if (p.length != 9 || p[4] > 3 || p[5] >= 8 || p[6] >= 8) return false;
        return switch (p[4]) {
          0 || 1 => p[6] == 0 && u16(7) == 0,
          2 => u16(7) == 0,
          3 =>
            p[6] == 0 &&
                u16(7) >= litchcraftAnimationMinimumFrameDurationMs &&
                u16(7) <= litchcraftAnimationMaximumFrameDurationMs,
          _ => false,
        };
      case streamBegin || replaceBeginOp:
        return p.length == 10 && u16(4) >= 32 && u16(4) <= maximumDocumentBytes;
      case streamChunk || replaceChunkOp:
        return p.length >= 5 &&
            u16(0) != 0 &&
            u16(2) + p.length - 4 <= maximumDocumentBytes;
      case streamCommit || replaceCommitOp || cancelTransfer:
        return p.length == 2 && u16(0) != 0;
      case streamCheckpoint:
        return p.length == 4 && u16(0) != 0 && u16(2) <= maximumDocumentBytes;
      default:
        return false;
    }
  }

  static Uint8List replaceBegin({
    required int session,
    required int request,
    required int baseRevision,
    required int length,
    required int crc32,
  }) {
    if (baseRevision < 0 ||
        baseRevision > 0xffffffff ||
        length < 32 ||
        length > maximumDocumentBytes ||
        crc32 < 0 ||
        crc32 > 0xffffffff) {
      throw ArgumentError('Invalid Litchcraft v2 replacement begin');
    }
    final data = ByteData(10)
      ..setUint32(0, baseRevision, Endian.little)
      ..setUint16(4, length, Endian.little)
      ..setUint32(6, crc32, Endian.little);
    return LitchcraftV2Protocol.request(
      replaceBeginOp,
      session,
      request,
      data.buffer.asUint8List(),
    );
  }

  static List<Uint8List> replaceChunks({
    required int session,
    required int requestStart,
    required int transfer,
    required List<int> document,
    required int maximumPacketBytes,
  }) {
    if (transfer < 1 ||
        transfer > 0xffff ||
        document.isEmpty ||
        document.length > maximumDocumentBytes ||
        document.any((int byte) => byte < 0 || byte > 0xff) ||
        maximumPacketBytes < minimumPacketBytes ||
        maximumPacketBytes > LitchcraftV2Protocol.maximumPacketBytes) {
      throw ArgumentError('Invalid Litchcraft v2 replacement chunks');
    }
    final bytesPerChunk = maximumPacketBytes - 13;
    final frames = <Uint8List>[];
    var request = requestStart;
    for (var offset = 0; offset < document.length; offset += bytesPerChunk) {
      final count = (document.length - offset).clamp(0, bytesPerChunk);
      final payload = <int>[
        transfer & 0xff,
        transfer >> 8,
        offset & 0xff,
        offset >> 8,
        ...document.skip(offset).take(count),
      ];
      frames.add(
        LitchcraftV2Protocol.request(replaceChunkOp, session, request, payload),
      );
      request = request == 0xffff ? 1 : request + 1;
    }
    return frames;
  }
}

class LitchcraftV2Packet {
  const LitchcraftV2Packet(
    this.event,
    this.session,
    this.revision,
    this.requestId,
    this.payload,
  );

  final int event, session, revision, requestId;
  final List<int> payload;

  static LitchcraftV2Packet? decode(List<int> bytes) {
    final maximumBytes =
        bytes.length >= 3 &&
            bytes[2] == LitchcraftV2Protocol.frameStoreSnapshotChunk
        ? 498
        : LitchcraftV2Protocol.maximumPacketBytes;
    if (bytes.length < 13 ||
        bytes.length > maximumBytes ||
        bytes.any((int byte) => byte < 0 || byte > 0xff) ||
        bytes[0] != LitchcraftV2Protocol.magic ||
        bytes[1] != LitchcraftV2Protocol.version) {
      return null;
    }
    final payload = bytes.sublist(13);
    final valid = switch (bytes[2]) {
      LitchcraftV2Protocol.state =>
        payload.length == 5 &&
            payload[0] <= LitchcraftProtocol.statusEntryPending &&
            payload[1] & ~0x0f == 0 &&
            ((payload[2] == 0 && payload[3] == 0 && payload[4] == 0) ||
                (payload[2] >= 1 &&
                    payload[2] <= 8 &&
                    payload[3] < payload[2] &&
                    payload[4] == LitchcraftV2Protocol.schema)),
      LitchcraftV2Protocol.snapshotBegin =>
        payload.length == 7 &&
            payload[0] == LitchcraftV2Protocol.schema &&
            (payload[1] | payload[2] << 8) >= 32 &&
            (payload[1] | payload[2] << 8) <=
                LitchcraftV2Protocol.maximumDocumentBytes,
      LitchcraftV2Protocol.snapshotChunk => payload.length >= 3,
      LitchcraftV2Protocol.snapshotEnd ||
      LitchcraftV2Protocol.changed => payload.isEmpty,
      LitchcraftV2Protocol.timelineLow =>
        payload.length == 7 &&
            payload[2] & ~1 == 0 &&
            (payload[0] | payload[1] << 8) != 0,
      LitchcraftV2Protocol.timelineHigh =>
        payload.length == 7 &&
            (payload[0] | payload[1] << 8) != 0 &&
            payload[2] < 8,
      LitchcraftV2Protocol.effectClockLow ||
      LitchcraftV2Protocol.effectClockHigh =>
        payload.length == 7 &&
            payload[2] == 0 &&
            (payload[0] | payload[1] << 8) != 0,
      LitchcraftV2Protocol.frameStoreCheckpointReceipt =>
        payload.length == 5 &&
            payload[0] <= LitchcraftProtocol.statusEntryPending,
      LitchcraftV2Protocol.frameStoreState => _validFrameStoreState(payload),
      LitchcraftV2Protocol.frameStoreSnapshotChunk => _validFrameStoreSnapshot(
        payload,
      ),
      LitchcraftV2Protocol.physicalStoreBatch => _validPhysicalStoreBatch(
        payload,
      ),
      LitchcraftV2Protocol.frameStoreResidentsState =>
        payload.length >= 3 &&
            payload[0] <= LitchcraftProtocol.statusEntryPending &&
            (payload[1] == 0xff || payload[1] < 2) &&
            payload[2] <= 2 &&
            payload.length == 3 + payload[2] * 45 &&
            _validFrameStoreResidents(payload),
      _ => false,
    };
    if (!valid) return null;
    final data = ByteData.sublistView(Uint8List.fromList(bytes));
    return LitchcraftV2Packet(
      bytes[2],
      data.getUint32(3, Endian.little),
      data.getUint32(7, Endian.little),
      data.getUint16(11, Endian.little),
      payload,
    );
  }

  static bool _validFrameStoreSnapshot(List<int> payload) {
    if (payload.length < 47 ||
        payload[0] > LitchcraftProtocol.statusEntryPending) {
      return false;
    }
    if (payload[0] != 0) return true;
    final flags = payload[1];
    final live = flags & 64 != 0;
    final single = flags & 32 != 0;
    final layout = flags & 128 != 0;
    final header = live ? 65 : 47;
    final count = payload[3] | payload[4] << 8;
    if (payload.length <= header ||
        (!live && layout) ||
        (live && (single == layout || flags & 8 != 0)) ||
        (single && flags & 8 != 0) ||
        payload[2] < 1 ||
        payload[2] > 3 ||
        count < 1 ||
        count > 256 ||
        (payload[5] | payload[6] << 8) >= count) {
      return false;
    }
    if (live) {
      final data = ByteData.sublistView(Uint8List.fromList(payload));
      if (data.getUint32(47, Endian.little) == 0 ||
          data.getUint32(51, Endian.little) == 0 ||
          payload[59] > 3 ||
          payload[64] != 0 ||
          data.getUint16(62, Endian.little) != 0 ||
          (payload[59] != 0 && data.getUint16(60, Endian.little) > 255) ||
          (layout && data.getUint32(7, Endian.little) != count * 6)) {
        return false;
      }
    }
    return true;
  }

  static bool _validPhysicalStoreBatch(List<int> payload) {
    if (payload.length < 23 || payload[0] & ~1 != 0) return false;
    final count = payload[1];
    if (count < 1 || count > 10 || payload.length != 2 + count * 21) {
      return false;
    }
    final data = ByteData.sublistView(Uint8List.fromList(payload));
    for (var index = 0; index < count; index++) {
      final offset = 2 + index * 21;
      final flags = payload[offset + 5];
      if (data.getUint32(offset, Endian.little) == 0 ||
          flags & 0x0f == 0 ||
          flags & ~0x1f != 0 ||
          data.getUint64(offset + 9, Endian.little) == 0 ||
          data.getUint32(offset + 17, Endian.little) == 0) {
        return false;
      }
    }
    return true;
  }
}

bool _validFrameStoreResidents(List<int> payload) {
  return _validFrameStoreResidentDescriptors(
    payload,
    selected: payload[1],
    count: payload[2],
    descriptorOffset: 3,
  );
}

bool _validFrameStoreState(List<int> payload) {
  final baseValid =
      payload.length >= 6 &&
      payload[0] <= LitchcraftProtocol.statusEntryPending &&
      payload[1] & ~0x3f == 0 &&
      (payload[1] & 2 == 0 || payload[1] & 1 != 0) &&
      ((payload[1] >> 3) & 3) < 3 &&
      (payload[1] & 4 == 0
          ? (payload[2] | payload[3] << 8) == 0 &&
                (payload[4] | payload[5] << 8) == 0
          : (payload[2] | payload[3] << 8) >= 1 &&
                (payload[2] | payload[3] << 8) <= 256 &&
                (payload[4] | payload[5] << 8) <
                    (payload[2] | payload[3] << 8));
  if (!baseValid) return false;
  if (payload.length == 6) return true;
  if (payload.length < 8) return false;
  final selected = payload[6];
  final count = payload[7];
  if ((selected != 0xff && selected >= 2) ||
      count > 2 ||
      payload.length != 8 + count * 45) {
    return false;
  }
  return _validFrameStoreResidentDescriptors(
    payload,
    selected: selected,
    count: count,
    descriptorOffset: 8,
  );
}

bool _validFrameStoreResidentDescriptors(
  List<int> payload, {
  required int selected,
  required int count,
  required int descriptorOffset,
}) {
  var selectedPresent = selected == 0xff;
  for (var index = 0; index < count; index++) {
    final offset = descriptorOffset + index * 45;
    final slot = payload[offset];
    final generation =
        payload[offset + 1] |
        payload[offset + 2] << 8 |
        payload[offset + 3] << 16 |
        payload[offset + 4] << 24;
    final frameCount = payload[offset + 13] | payload[offset + 14] << 8;
    if (slot > 1 ||
        generation == 0 ||
        frameCount < 1 ||
        frameCount > 256 ||
        payload[offset + 15] < 1 ||
        payload[offset + 15] > 3 ||
        payload[offset + 16] & ~7 != 0) {
      return false;
    }
    selectedPresent = selectedPresent || slot == selected;
  }
  return selectedPresent;
}

/// Runtime commands which do not mutate the schema-3 document.
enum LitchcraftV2TimelineAction { pause, play, selectFrame }

/// The immutable authority watermark carried by every v2 State acknowledgement.
class LitchcraftV2State {
  const LitchcraftV2State({
    required this.session,
    required this.revision,
    required this.status,
    required this.active,
    required this.playing,
    required this.canUndo,
    required this.canRedo,
    required this.frameCount,
    required this.selectedFrame,
    required this.schema,
  });

  final int session, revision, status, frameCount, selectedFrame, schema;
  final bool active, playing, canUndo, canRedo;

  factory LitchcraftV2State.fromPacket(LitchcraftV2Packet packet) {
    if (packet.event != LitchcraftV2Protocol.state) {
      throw ArgumentError.value(packet, 'packet', 'Expected a v2 State packet');
    }
    return LitchcraftV2State(
      session: packet.session,
      revision: packet.revision,
      status: packet.payload[0],
      active: packet.payload[1] & 1 != 0,
      playing: packet.payload[1] & 2 != 0,
      canUndo: packet.payload[1] & 4 != 0,
      canRedo: packet.payload[1] & 8 != 0,
      frameCount: packet.payload[2],
      selectedFrame: packet.payload[3],
      schema: packet.payload[4],
    );
  }
}

/// A complete board-clock sample. [extrapolatedElapsedAt] is deliberately a
/// pure helper: callers choose when to render and it never starts a poller.
class LitchcraftV2TimelineSample {
  const LitchcraftV2TimelineSample({
    required this.session,
    required this.revision,
    required this.sampleId,
    required this.playing,
    required this.selectedFrame,
    required this.elapsedMilliseconds,
    required this.receivedAtMonotonicMilliseconds,
    this.transferSuspended = false,
  });

  final int session, revision, sampleId, selectedFrame;
  final bool playing, transferSuspended;
  final int elapsedMilliseconds, receivedAtMonotonicMilliseconds;

  int extrapolatedElapsedAt(int monotonicMilliseconds) {
    if (!playing || transferSuspended) return elapsedMilliseconds;
    final elapsed = monotonicMilliseconds - receivedAtMonotonicMilliseconds;
    return elapsedMilliseconds + (elapsed < 0 ? 0 : elapsed);
  }
}

class LitchcraftV2EffectClockSample {
  const LitchcraftV2EffectClockSample({
    required this.session,
    required this.revision,
    required this.sampleId,
    required this.elapsedMilliseconds,
    required this.receivedAtMonotonicMilliseconds,
    this.transferSuspended = false,
  });
  final bool transferSuspended;
  final int session,
      revision,
      sampleId,
      elapsedMilliseconds,
      receivedAtMonotonicMilliseconds;
  int extrapolatedElapsedAt(int now) => transferSuspended
      ? elapsedMilliseconds
      : elapsedMilliseconds +
            (now - receivedAtMonotonicMilliseconds).clamp(
              0,
              0x7fffffffffffffff,
            );
}

class _V2TimelineHalf {
  _V2TimelineHalf(this.packet, this.isLow);

  final LitchcraftV2Packet packet;
  final bool isLow;
  Timer? timer;
}

class LitchcraftProtocol {
  static const magic = 0x4c;
  static const bulkReplace = 0x40;
  static const enter = 1,
      exit = 2,
      tool = 3,
      paint = 4,
      undo = 5,
      redo = 6,
      clear = 7,
      snapshot = 8,
      replaceBegin = 9,
      replaceChunk = 10,
      replaceCommit = 11,
      inspect = 12,
      paletteChunk = 13,
      shakeErase = 14;
  static const state = 0x81,
      brush = 0x82,
      cell = 0x83,
      snapshotBegin = 0x84,
      snapshotChunk = 0x85,
      snapshotEnd = 0x86,
      resyncRequired = 0x87,
      colorPick = 0x88,
      gesture = 0x89;
  static const stateActive = 1, stateWaitingForLift = 2;
  static const statusEntryPending = 12;
  // Static, advanced and Store owners retain at most 64 completed operations.
  // Legacy State can additionally expose the current uncommitted Undo stroke.
  static const maximumHistoryOperations = 64;
  static const colorPickIdle = 0, colorPickPicking = 1, colorPickPicked = 2;
  static const gestureShakeErase = 1;

  static Uint8List request(
    int opcode,
    int session,
    int id, [
    List<int> payload = const [],
  ]) {
    if (opcode < 1 ||
        opcode > 14 ||
        session < 0 ||
        session > 0xffffffff ||
        id < 1 ||
        id > 0xffff ||
        payload.length > 12 ||
        payload.any((v) => v < 0 || v > 255)) {
      throw ArgumentError('Invalid Litchcraft request');
    }
    final bytes = Uint8List(8 + payload.length);
    final data = ByteData.sublistView(bytes);
    bytes[0] = magic;
    bytes[1] = opcode;
    data.setUint32(2, session, Endian.little);
    data.setUint16(6, id, Endian.little);
    bytes.setRange(8, bytes.length, payload);
    return bytes;
  }

  static List<int> rgb(int color) => [
    (color >> 16) & 255,
    (color >> 8) & 255,
    color & 255,
  ];
  static int readRgb(List<int> bytes, int offset) =>
      bytes[offset] << 16 | bytes[offset + 1] << 8 | bytes[offset + 2];
  static List<int> u16(int value) => [value & 255, (value >> 8) & 255];

  /// High-MTU frame: magic/op/session/request/element offset/final/count/RGB.
  /// At Android's requested MTU 247, a complete 768-byte canvas is four writes.
  static List<Uint8List> bulkReplaceFrames({
    required int session,
    required int request,
    required List<int> colors,
    required int maximumPacketBytes,
  }) {
    if (session < 1 ||
        session > 0xffffffff ||
        request < 1 ||
        request > 0xffff ||
        colors.length != 256 ||
        colors.any((color) => color < 0 || color > 0xffffff) ||
        maximumPacketBytes < 15 ||
        maximumPacketBytes > 512) {
      throw ArgumentError('Invalid Litchcraft bulk replacement');
    }
    final elementsPerFrame = ((maximumPacketBytes - 12) ~/ 3).clamp(1, 255);
    final frames = <Uint8List>[];
    for (var offset = 0; offset < colors.length; offset += elementsPerFrame) {
      final count = (colors.length - offset).clamp(0, elementsPerFrame);
      final bytes = Uint8List(12 + count * 3);
      final data = ByteData.sublistView(bytes);
      bytes[0] = magic;
      bytes[1] = bulkReplace;
      data.setUint32(2, session, Endian.little);
      data.setUint16(6, request, Endian.little);
      data.setUint16(8, offset, Endian.little);
      bytes[10] = offset + count == colors.length ? 1 : 0;
      bytes[11] = count;
      var destination = 12;
      for (final color in colors.skip(offset).take(count)) {
        bytes.setRange(destination, destination + 3, rgb(color));
        destination += 3;
      }
      frames.add(bytes);
    }
    return frames;
  }
}

class LitchcraftPacket {
  const LitchcraftPacket(
    this.event,
    this.session,
    this.revision,
    this.requestId,
    this.payload,
  );
  final int event, session, revision, requestId;
  final List<int> payload;

  static LitchcraftPacket? decode(List<int> bytes) {
    if (bytes.length < 12 ||
        bytes.length > 244 ||
        bytes.any((v) => v < 0 || v > 255) ||
        bytes[0] != LitchcraftProtocol.magic) {
      return null;
    }
    final payload = bytes.sublist(12);
    final valid = switch (bytes[1]) {
      LitchcraftProtocol.state || LitchcraftProtocol.snapshotBegin =>
        payload.length == 8 &&
            payload[0] <= LitchcraftProtocol.statusEntryPending &&
            payload[1] <= 3 &&
            payload[2] >= 1 &&
            payload[2] <= 3 &&
            payload[3] <= LitchcraftProtocol.maximumHistoryOperations + 1 &&
            payload[4] <= LitchcraftProtocol.maximumHistoryOperations &&
            payload[5] <= 1 &&
            payload[6] >= 1 &&
            payload[6] <= 15,
      LitchcraftProtocol.brush =>
        payload.length == 5 &&
            payload[3] <= 1 &&
            payload[4] >= 1 &&
            payload[4] <= 15,
      LitchcraftProtocol.cell =>
        payload.length == 5 &&
            payload[0] < 64 &&
            payload[1] >= 1 &&
            payload[1] <= 15,
      LitchcraftProtocol.snapshotChunk =>
        payload.length >= 5 &&
            (payload.length - 2) % 3 == 0 &&
            payload[0] + (payload[1] << 8) + (payload.length - 2) ~/ 3 <= 256,
      LitchcraftProtocol.snapshotEnd ||
      LitchcraftProtocol.resyncRequired => payload.isEmpty,
      LitchcraftProtocol.colorPick =>
        payload.length == 1 && payload[0] <= LitchcraftProtocol.colorPickPicked,
      LitchcraftProtocol.gesture =>
        payload.length == 3 &&
            payload[0] == LitchcraftProtocol.gestureShakeErase &&
            (payload[1] | payload[2] << 8) != 0,
      _ => false,
    };
    if (!valid) return null;
    final data = ByteData.sublistView(Uint8List.fromList(bytes));
    return LitchcraftPacket(
      bytes[1],
      data.getUint32(2, Endian.little),
      data.getUint32(6, Endian.little),
      data.getUint16(10, Endian.little),
      payload,
    );
  }
}

class LitchcraftRejected implements Exception {
  const LitchcraftRejected(this.status);
  final int status;
  @override
  String toString() => 'Litchcraft request rejected ($status)';
}

class LitchcraftResyncRequired implements Exception {
  const LitchcraftResyncRequired();
}

/// Negotiation cannot carry bulk; no artwork bytes have been sent.
class LitchcraftBulkUnavailable implements Exception {
  const LitchcraftBulkUnavailable();
}

/// Shared-session firmware must advertise the authoritative mode lane.
class LitchcraftModeUnavailable implements Exception {
  const LitchcraftModeUnavailable();
}

class _PendingRequest {
  _PendingRequest(this.opcode, this.session);
  final int opcode;
  final int session;
  final completer = Completer<LitchcraftPacket>();
  final settled = Completer<void>();
  bool cancelled = false;
}

class _PendingV2Request {
  _PendingV2Request(this.operation, this.session);
  final int operation;
  final int session;
  final completer = Completer<LitchcraftV2Packet>();
  final writeRetirement = Completer<void>();
  bool cancelled = false;

  void retireWrites() {
    if (!writeRetirement.isCompleted) writeRetirement.complete();
  }
}

typedef LitchcraftLinkDiagnostic =
    void Function(String event, Map<String, Object?> fields);

/// One connection generation owns one typed notification subscription. Writes
/// are serialized by the controller; retries reuse the exact request bytes.
class LitchcraftLink {
  static const Duration _firstAckWindow = Duration(milliseconds: 250);
  static const Duration _secondAckWindow = Duration(milliseconds: 750);

  LitchcraftLink({
    required Stream<List<int>> packets,
    required this.write,
    this.writeUnreliable,
    this.writeBulkArtwork,
    this.writeBulkDocumentPacket,
    required this.onPacket,
    required this.onFailure,
    this.onV2Packet,
    this.onDiagnostic,
    this.allocateRequestId,
    this.monotonicMilliseconds,
    this.timelineHalfTimeout = const Duration(milliseconds: 250),
    this.timelineTimer = Timer.new,
    this.timeout = const Duration(seconds: 6),
  }) {
    _subscription = packets.listen(
      _receive,
      onError: (Object error) => _fail(error),
      onDone: () => _fail(StateError('Litchcraft disconnected')),
    );
  }
  final Future<void> Function(List<int>) write;
  final Future<void> Function(List<int>)? writeUnreliable;
  final Future<void> Function(List<int>)? writeBulkDocumentPacket;
  bool? _bulkDocumentSupported;
  bool? _bulkDocumentWindowSupported;
  final Future<void> Function(int session, int request, List<int> colors)?
  writeBulkArtwork;
  final void Function(LitchcraftPacket) onPacket;
  final void Function(LitchcraftV2Packet)? onV2Packet;
  final void Function(Object) onFailure;
  final LitchcraftLinkDiagnostic? onDiagnostic;
  final Duration timeout;
  final int Function()? allocateRequestId;
  final int Function()? monotonicMilliseconds;
  final Duration timelineHalfTimeout;
  final Timer Function(Duration, void Function()) timelineTimer;
  late final StreamSubscription<List<int>> _subscription;
  final _timelineSamples =
      StreamController<LitchcraftV2TimelineSample>.broadcast(sync: true);
  // Process a document packet inline: no paused broadcast subscription can
  // retain an unbounded queue of notification descriptors or document copies.
  _V2DocumentTransfer? _documentTransfer;
  bool _v2RequestBusy = false;
  int? _v2Session, _changedSession, _changedRevision;
  _V2TimelineHalf? _timelineHalf;
  _V2TimelineHalf? _effectClockHalf;
  LitchcraftV2EffectClockSample? _effectClock;
  LitchcraftV2EffectClockSample? get effectClock => _effectClock;
  bool? _effectClockSupported;
  int? _effectClockRefreshRequest;
  int? _timelineRecoverySession, _timelineRecoveryRevision;
  bool _timelineRecoveryInFlight = false;
  late final Stopwatch _monotonicStopwatch = Stopwatch()..start();

  /// Complete, same-epoch Timeline samples. The stream never exposes a lone
  /// low/high half, so consumers cannot accidentally compose two board clocks.
  Stream<LitchcraftV2TimelineSample> get v2TimelineSamples =>
      _timelineSamples.stream;

  int _monotonicNow() =>
      monotonicMilliseconds?.call() ?? _monotonicStopwatch.elapsedMilliseconds;

  /// A connection-owned watermark survives End, client disposal and idle gaps.
  /// It never substitutes for a validated complete document.
  int? documentChangedRevision(int session) =>
      _changedSession == session ? _changedRevision : null;
  final Map<int, _PendingRequest> _pending = {};
  final Map<int, _PendingV2Request> _pendingV2 = {};
  int _nextId = 0;
  bool _closed = false;
  int _modeEpoch = 0;

  /// A request releases its cancellation listeners after native and ACK join,
  /// or immediately when its unissued suffix is retired by mode/link lifetime.
  Future<void> v2WriteRetirement(List<int> bytes) {
    final request = bytes.length >= 9 ? bytes[7] | bytes[8] << 8 : -1;
    return _pendingV2[request]?.writeRetirement.future ?? Future<void>.value();
  }

  bool Function() v2WriteValidity(List<int> bytes) {
    final epoch = _modeEpoch;
    final request = bytes.length >= 9 ? bytes[7] | bytes[8] << 8 : -1;
    final pending = _pendingV2[request];
    // Retain the request object: a valid ACK can remove its map entry before
    // an already-issued SDK write returns, without cancelling that obligation.
    return () =>
        !_closed &&
        epoch == _modeEpoch &&
        pending != null &&
        !pending.cancelled;
  }

  Future<void> _waitForTool() async {
    while (true) {
      final tools = _pending.values
          .where((p) => p.opcode == LitchcraftProtocol.tool)
          .toList();
      if (tools.isEmpty) return;
      await Future.wait(tools.map((tool) => tool.settled.future));
    }
  }

  void _diagnose(String event, Map<String, Object?> fields) {
    onDiagnostic?.call(event, fields);
  }

  Future<void> sendUnacknowledged(
    int opcode,
    int session, [
    List<int> payload = const [],
  ]) async {
    if (_closed ||
        _v2RequestBusy ||
        _pending.isNotEmpty ||
        _documentTransfer != null) {
      throw StateError('Litchcraft link closed or busy');
    }
    final modeEpoch = _modeEpoch;
    _nextId = (_nextId % 0xffff) + 1;
    final id = allocateRequestId?.call() ?? _nextId;
    final bytes = LitchcraftProtocol.request(opcode, session, id, payload);
    await Future<void>.value();
    if (modeEpoch != _modeEpoch) throw const LitchcraftTransferCancelled();
    if (_closed) throw StateError('Litchcraft link closed');
    await (writeUnreliable ?? write)(bytes).timeout(timeout);
  }

  void _receive(List<int> bytes) {
    if (bytes.isNotEmpty && bytes[0] == LitchcraftV2Protocol.magic) {
      _receiveV2(bytes);
      return;
    }
    final packet = LitchcraftPacket.decode(bytes);
    if (_closed || packet == null) return;
    if (packet.event == LitchcraftProtocol.state) {
      final pending = _pending[packet.requestId];
      if (pending != null &&
          packet.payload[0] == 0 &&
          pending.session != 0 &&
          pending.session != packet.session) {
        _diagnose('packet_rejected', <String, Object?>{
          'packet_kind': 'state',
          'reason': 'wrong_session',
          'opcode': pending.opcode,
          'request_id': packet.requestId,
          'session': packet.session,
          'expected_session': pending.session,
          'revision': packet.revision,
        });
        return;
      }
      if (packet.requestId != 0 && pending == null) {
        _diagnose('packet_rejected', <String, Object?>{
          'packet_kind': 'state',
          'reason': 'no_pending_request',
          'request_id': packet.requestId,
          'session': packet.session,
          'revision': packet.revision,
        });
        return;
      }
      onPacket(packet);
      _pending.remove(packet.requestId);
      if (pending != null && !pending.completer.isCompleted) {
        pending.completer.complete(packet);
      }
      return;
    }
    onPacket(packet);
    if (packet.event == LitchcraftProtocol.resyncRequired) {
      final interrupted = _pending.entries
          .where(
            (entry) =>
                entry.value.session == 0 ||
                entry.value.session == packet.session,
          )
          .toList();
      for (final entry in interrupted) {
        final pending = entry.value;
        if (!pending.completer.isCompleted) {
          pending.completer.completeError(const LitchcraftResyncRequired());
        }
        _pending.remove(entry.key);
      }
    }
  }

  void _receiveV2(List<int> bytes) {
    final packet = LitchcraftV2Packet.decode(bytes);
    if (_closed || packet == null) return;
    if (packet.event == LitchcraftV2Protocol.state && packet.requestId != 0) {
      final pending = _pendingV2[packet.requestId];
      if (pending == null ||
          (pending.session != 0 &&
              pending.session != packet.session &&
              packet.payload[0] != 2)) {
        if (pending == null && packet.payload[0] != 0) {
          // Additive frame-store probes use the connection-owned transfer
          // channel. Older firmware answers them with a legacy V2 refusal;
          // its owner still validates the exact session and request.
          _documentTransfer?.receive(packet);
        }
        _diagnose('packet_rejected', <String, Object?>{
          'packet_kind': 'v2_state',
          'reason': pending == null ? 'no_pending_request' : 'wrong_session',
          'request_id': packet.requestId,
          'session': packet.session,
        });
        return;
      }
      _pendingV2.remove(packet.requestId);
      if (!pending.completer.isCompleted) pending.completer.complete(packet);
    }
    if (packet.event == LitchcraftV2Protocol.state && packet.payload[0] == 0) {
      if (_v2Session != null && _v2Session != packet.session) {
        _timelineHalf?.timer?.cancel();
        _timelineHalf = null;
        _timelineRecoverySession = null;
        _timelineRecoveryRevision = null;
        _effectClockHalf = null;
        _effectClock = null;
        _effectClockRefreshRequest = null;
      }
      _v2Session = packet.session;
      if (_changedSession != packet.session) {
        _changedSession = null;
        _changedRevision = null;
      }
    } else if (packet.event == LitchcraftV2Protocol.changed &&
        packet.requestId == 0 &&
        (_v2Session == null || _v2Session == packet.session)) {
      if (_changedSession != packet.session ||
          _changedRevision == null ||
          LitchcraftV2DocumentClient._newer(
            packet.revision,
            _changedRevision!,
          )) {
        _changedSession = packet.session;
        _changedRevision = packet.revision;
      }
    }
    if (packet.event == LitchcraftV2Protocol.timelineLow ||
        packet.event == LitchcraftV2Protocol.timelineHigh) {
      _receiveTimelineHalf(packet);
    }
    if (packet.event == LitchcraftV2Protocol.effectClockLow ||
        packet.event == LitchcraftV2Protocol.effectClockHigh) {
      _receiveEffectClockHalf(packet);
    }
    _documentTransfer?.receive(packet);
    onV2Packet?.call(packet);
  }

  void _receiveEffectClockHalf(LitchcraftV2Packet packet) {
    if (_v2Session != packet.session ||
        (_effectClockRefreshRequest != null &&
            (_effectClockRefreshRequest == 0 ||
                packet.requestId != _effectClockRefreshRequest))) {
      return;
    }
    final isLow = packet.event == LitchcraftV2Protocol.effectClockLow;
    final sampleId = packet.payload[0] | packet.payload[1] << 8;
    final current = _effectClock;
    if (current != null) {
      final delta = (sampleId - current.sampleId) & 0xffff;
      if (delta == 0 || delta >= 0x8000) return;
    }
    final pending = _effectClockHalf;
    if (pending == null ||
        pending.isLow == isLow ||
        pending.packet.session != packet.session ||
        pending.packet.revision != packet.revision ||
        pending.packet.requestId != packet.requestId ||
        (pending.packet.payload[0] | pending.packet.payload[1] << 8) !=
            sampleId) {
      _effectClockHalf = _V2TimelineHalf(packet, isLow);
      return;
    }
    _effectClockHalf = null;
    // The exact refresh pair establishes this source's sample floor. Later
    // opted-in Inspect/playback pairs advance it without accepting old halves.
    _effectClockRefreshRequest = null;
    final low = isLow ? packet : pending.packet;
    final high = isLow ? pending.packet : packet;
    int value(LitchcraftV2Packet p) => ByteData.sublistView(
      Uint8List.fromList(p.payload),
    ).getUint32(3, Endian.little);
    _effectClock = LitchcraftV2EffectClockSample(
      session: packet.session,
      revision: packet.revision,
      sampleId: sampleId,
      elapsedMilliseconds: value(low) | value(high) << 32,
      receivedAtMonotonicMilliseconds: _monotonicNow(),
    );
  }

  void _receiveTimelineHalf(LitchcraftV2Packet packet) {
    // A Timeline belongs to the State-established session for this connection.
    // Before State, or after a new State session, a delayed old epoch is noise.
    if (_v2Session != packet.session) {
      _diagnose('timeline_dropped', <String, Object?>{
        'reason': 'stale_session',
        'session': packet.session,
        'expected_session': _v2Session,
      });
      return;
    }
    final isLow = packet.event == LitchcraftV2Protocol.timelineLow;
    final sampleId = packet.payload[0] | packet.payload[1] << 8;
    final pending = _timelineHalf;
    if (pending == null) {
      _storeTimelineHalf(packet, isLow);
      return;
    }
    final pendingId =
        pending.packet.payload[0] | pending.packet.payload[1] << 8;
    if (pending.isLow == isLow ||
        pendingId != sampleId ||
        pending.packet.session != packet.session ||
        pending.packet.revision != packet.revision) {
      _diagnose('timeline_dropped', <String, Object?>{
        'reason': 'mismatched_half',
        'sample_id': sampleId,
        'pending_sample_id': pendingId,
        'session': packet.session,
        'revision': packet.revision,
      });
      return;
    }
    pending.timer?.cancel();
    _timelineHalf = null;
    _timelineRecoverySession = null;
    _timelineRecoveryRevision = null;
    final low = isLow ? packet : pending.packet;
    final high = isLow ? pending.packet : packet;
    final lowElapsed = ByteData.sublistView(
      Uint8List.fromList(low.payload),
    ).getUint32(3, Endian.little);
    final highElapsed = ByteData.sublistView(
      Uint8List.fromList(high.payload),
    ).getUint32(3, Endian.little);
    _timelineSamples.add(
      LitchcraftV2TimelineSample(
        session: packet.session,
        revision: packet.revision,
        sampleId: sampleId,
        playing: low.payload[2] & 1 != 0,
        selectedFrame: high.payload[2],
        elapsedMilliseconds: lowElapsed | highElapsed << 32,
        receivedAtMonotonicMilliseconds: _monotonicNow(),
      ),
    );
  }

  void _storeTimelineHalf(LitchcraftV2Packet packet, bool isLow) {
    final half = _V2TimelineHalf(packet, isLow);
    _timelineHalf = half;
    half.timer = timelineTimer(timelineHalfTimeout, () {
      if (!identical(_timelineHalf, half)) return;
      _timelineHalf = null;
      final alreadyRecovered =
          _timelineRecoverySession == packet.session &&
          _timelineRecoveryRevision == packet.revision;
      if (alreadyRecovered || _closed) {
        _diagnose('timeline_dropped', <String, Object?>{
          'reason': alreadyRecovered ? 'recovery_exhausted' : 'link_closed',
          'session': packet.session,
          'revision': packet.revision,
        });
        return;
      }
      _timelineRecoverySession = packet.session;
      _timelineRecoveryRevision = packet.revision;
      _diagnose('timeline_recovery', <String, Object?>{
        'session': packet.session,
        'revision': packet.revision,
      });
      _startTimelineRecovery();
    });
  }

  void _startTimelineRecovery() {
    final session = _timelineRecoverySession;
    final revision = _timelineRecoveryRevision;
    if (session == null ||
        revision == null ||
        _timelineRecoveryInFlight ||
        _closed ||
        _v2RequestBusy ||
        _pending.isNotEmpty ||
        _documentTransfer != null) {
      return;
    }
    _timelineRecoveryInFlight = true;
    unawaited(
      inspectV2(session: session)
          .then<void>(
            (_) {},
            onError: (Object error, StackTrace stackTrace) {
              _diagnose('timeline_recovery_failed', <String, Object?>{
                'session': session,
                'revision': revision,
                'kind': error.runtimeType.toString(),
              });
            },
          )
          .whenComplete(() {
            _timelineRecoveryInFlight = false;
          }),
    );
  }

  Future<LitchcraftPacket> send(
    int opcode,
    int session, [
    List<int> payload = const [],
    void Function(int)? onRequest,
  ]) async {
    final modeControl =
        opcode == LitchcraftProtocol.enter || opcode == LitchcraftProtocol.exit;
    final toolControl = opcode == LitchcraftProtocol.tool;
    if (!modeControl && !toolControl) await _waitForTool();
    if (_closed ||
        (!modeControl &&
            !toolControl &&
            (_v2RequestBusy ||
                _pending.values.any(
                  (p) => p.opcode != LitchcraftProtocol.tool,
                ) ||
                _documentTransfer != null))) {
      throw StateError('Litchcraft link closed or busy');
    }
    if (modeControl) {
      ++_modeEpoch;
      // Mode authority outranks document work. Already submitted native writes
      // retain SDK ordering; no further old chunks or retries may be submitted.
      final transfer = _documentTransfer;
      if (transfer != null) {
        transfer.supersededByMode = true;
        transfer.abort(const LitchcraftTransferCancelled());
      }
      for (final pending in _pending.values) {
        pending.cancelled = true;
        if (!pending.completer.isCompleted) {
          pending.completer.completeError(const LitchcraftTransferCancelled());
        }
      }
      for (final pending in _pendingV2.values) {
        pending.cancelled = true;
        pending.retireWrites();
        if (!pending.completer.isCompleted) {
          pending.completer.completeError(const LitchcraftTransferCancelled());
        }
      }
      _pending.clear();
      _pendingV2.clear();
    }
    _nextId = (_nextId % 0xffff) + 1;
    final id = allocateRequestId?.call() ?? _nextId;
    final bytes = LitchcraftProtocol.request(opcode, session, id, payload);
    final pending = _PendingRequest(opcode, session);
    _pending[id] = pending;
    // Listen for a synchronous failure before the native write can complete.
    final outcome = pending.completer.future;
    unawaited(
      outcome.then<void>((_) {}, onError: (Object error, StackTrace stack) {}),
    );
    try {
      onRequest?.call(id);
      _diagnose('request_start', <String, Object?>{
        'opcode': opcode,
        'request_id': id,
        'session': session,
      });
      // Yield without installing a Timer. A controller can be disposed while a
      // queued stroke is at this boundary, and a zero-duration Timer would then
      // outlive the screen even though no transport work had started.
      await Future<void>.value();
      for (var attempt = 0; attempt < 3; attempt++) {
        try {
          if (_closed || pending.cancelled) {
            throw const LitchcraftTransferCancelled();
          }
          await write(bytes).timeout(timeout);
          final ack = await outcome.timeout(_ackWindow(attempt));
          if (ack.payload[0] != 0) throw LitchcraftRejected(ack.payload[0]);
          _diagnose('request_terminal', <String, Object?>{
            'opcode': opcode,
            'request_id': id,
            'session': ack.session,
            'revision': ack.revision,
            'attempt': attempt + 1,
            'outcome': 'acknowledged',
          });
          return ack;
        } on TimeoutException {
          if (pending.cancelled) throw const LitchcraftTransferCancelled();
          if (_closed || attempt == 2) {
            final error = TimeoutException(
              'Litchcraft notification acknowledgement timed out',
              timeout,
            );
            _fail(error);
            throw error;
          }
          _diagnose('request_retry', <String, Object?>{
            'opcode': opcode,
            'request_id': id,
            'session': session,
            'attempt': attempt + 2,
            'reason': 'ack_timeout',
          });
        }
      }
      throw StateError('Unreachable Litchcraft retry');
    } catch (error) {
      _diagnose('request_terminal', <String, Object?>{
        'opcode': opcode,
        'request_id': id,
        'session': session,
        'outcome': error is LitchcraftRejected
            ? 'rejected'
            : error is TimeoutException
            ? 'timeout'
            : 'failed',
        if (error is LitchcraftRejected) 'status': error.status,
        'kind': error.runtimeType.toString(),
      });
      rethrow;
    } finally {
      _pending.remove(id);
      pending.settled.complete();
    }
  }

  /// Uses the same connection-scoped subscription and request allocator as
  /// static Litchcraft. Callers serialize document operations; this link never
  /// opens a second 0106 subscription or changes capability discovery.
  Future<LitchcraftV2Packet> sendV2(
    int operation,
    int session, [
    List<int> payload = const <int>[],
    void Function(int)? onRequest,
  ]) => _sendV2(operation, session, payload, onRequest);

  /// Reads v2 document metadata and asks the board for one frozen Timeline
  /// sample. Subscribe to [v2TimelineSamples] before calling when that sample
  /// matters to the immediate presentation.
  Future<LitchcraftV2State> inspectV2({
    int session = 0,
    bool refreshEffectClock = false,
  }) async {
    if (_effectClockSupported == null ||
        refreshEffectClock && _effectClockSupported != false) {
      if (refreshEffectClock) {
        // Close clock admission before request allocation can await a Tool.
        _effectClockRefreshRequest = 0;
        _effectClockHalf = null;
        _effectClock = null;
      }
      try {
        final packet = await sendV2(
          LitchcraftV2Protocol.inspect,
          session,
          const [1],
          refreshEffectClock
              ? (request) {
                  // A new document epoch can reuse the same mode session. Only this
                  // requested frozen pair may reanchor it; delayed old halves cannot.
                  _effectClockRefreshRequest = request;
                  _effectClockHalf = null;
                  _effectClock = null;
                }
              : null,
        );
        _effectClockSupported = true;
        return LitchcraftV2State.fromPacket(packet);
      } on LitchcraftRejected catch (error) {
        if (error.status != 3 && error.status != 4) rethrow;
        _effectClockSupported = false;
      }
    }
    return _v2State(LitchcraftV2Protocol.inspect, session, const <int>[]);
  }

  int? _canvasEditProbeSession;
  bool _canvasEditSupported = false;
  int? _canvasRotateProbeSession;
  bool _canvasRotateSupported = false;
  int? _canvasPatternProbeSession;
  bool _canvasPatternSupported = false;

  Future<bool> supportsCanvasRotateV2({required int session}) async {
    if (_canvasRotateProbeSession == session) return _canvasRotateSupported;
    try {
      await _v2State(LitchcraftV2Protocol.canvasEdit, session, const [2]);
      _canvasRotateProbeSession = session;
      return _canvasRotateSupported = true;
    } on LitchcraftRejected catch (error) {
      if (error.status == 3 || error.status == 4) {
        _canvasRotateProbeSession = session;
        return _canvasRotateSupported = false;
      }
      rethrow;
    }
  }

  Future<bool> supportsCanvasEditV2({required int session}) async {
    if (_canvasEditProbeSession == session) return _canvasEditSupported;
    try {
      await _v2State(LitchcraftV2Protocol.canvasEdit, session, const <int>[]);
      _canvasEditProbeSession = session;
      return _canvasEditSupported = true;
    } on LitchcraftRejected catch (error) {
      if (error.status == 4) {
        _canvasEditProbeSession = session;
        return _canvasEditSupported = false;
      }
      rethrow;
    }
  }

  Future<bool> supportsCanvasPatternV2({required int session}) async {
    if (_canvasPatternProbeSession == session) return _canvasPatternSupported;
    try {
      await _v2State(LitchcraftV2Protocol.canvasEdit, session, const [3]);
      _canvasPatternProbeSession = session;
      _canvasEditProbeSession = session;
      _canvasEditSupported = true;
      _canvasRotateProbeSession = session;
      _canvasRotateSupported = true;
      return _canvasPatternSupported = true;
    } on LitchcraftRejected catch (error) {
      if (error.status == 3 || error.status == 4) {
        _canvasPatternProbeSession = session;
        return _canvasPatternSupported = false;
      }
      rethrow;
    }
  }

  Future<LitchcraftV2State> canvasEditV2({
    required int session,
    required int baseRevision,
    required int frame,
    required int mask,
    required int color,
    required bool erase,
    int quarterTurns = 0,
  }) {
    if (baseRevision < 0 ||
        baseRevision > 0xffffffff ||
        color < 0 ||
        color > 0xffffff) {
      throw ArgumentError('Invalid Litchcraft canvas edit');
    }
    return _v2State(LitchcraftV2Protocol.canvasEdit, session, <int>[
      for (var shift = 0; shift < 32; shift += 8)
        (baseRevision >> shift) & 0xff,
      frame,
      mask,
      ...LitchcraftProtocol.rgb(color),
      quarterTurns == 0 ? (erase ? 1 : 0) : 1 + quarterTurns,
    ]);
  }

  Future<LitchcraftV2State> checkerEditV2({
    required int session,
    required int baseRevision,
    required int frame,
    required int mask,
    required int color,
    required bool dark,
    required bool erase,
  }) {
    if (baseRevision < 0 ||
        baseRevision > 0xffffffff ||
        color < 0 ||
        color > 0xffffff) {
      throw ArgumentError('Invalid Litchcraft checker edit');
    }
    return _v2State(LitchcraftV2Protocol.canvasEdit, session, <int>[
      for (var shift = 0; shift < 32; shift += 8)
        (baseRevision >> shift) & 0xff,
      frame,
      mask,
      ...LitchcraftProtocol.rgb(erase ? 0 : color),
      5 + (dark ? 1 : 0) + (erase ? 2 : 0),
    ]);
  }

  Future<LitchcraftV2State> pauseV2({
    required int session,
    required int frame,
  }) => _timelineV2(LitchcraftV2TimelineAction.pause, session, frame);

  Future<LitchcraftV2State> playV2({
    required int session,
    required int frame,
  }) => _timelineV2(LitchcraftV2TimelineAction.play, session, frame);

  Future<LitchcraftV2State> selectFrameV2({
    required int session,
    required int frame,
  }) => _timelineV2(LitchcraftV2TimelineAction.selectFrame, session, frame);

  Future<LitchcraftV2State> paintV2({
    required int session,
    required int frame,
    required int stroke,
    required int flags,
    required int mask,
    required int color,
    required List<int> squares,
  }) {
    _validateEdit(session, frame, stroke, flags, mask, squares);
    if (color < 0 || color > 0xffffff) {
      throw ArgumentError.value(color, 'color');
    }
    return _v2State(LitchcraftV2Protocol.paint, session, <int>[
      frame,
      stroke & 0xff,
      stroke >> 8,
      flags,
      mask,
      color >> 16,
      color >> 8 & 0xff,
      color & 0xff,
      ...squares,
    ]);
  }

  Future<LitchcraftV2State> effectV2({
    required int session,
    required int frame,
    required int effect,
    required int durationMs,
    required int stroke,
    required int flags,
    required int mask,
    required List<int> squares,
  }) {
    _validateEdit(session, frame, stroke, flags, mask, squares);
    if (effect < 0 ||
        effect > 8 ||
        (effect == 0
            ? durationMs != 0
            : durationMs < 500 ||
                  durationMs > 10000 ||
                  durationMs % 100 != 0)) {
      throw ArgumentError('Invalid v2 effect or effect duration');
    }
    return _v2State(LitchcraftV2Protocol.effect, session, <int>[
      frame,
      effect,
      durationMs & 0xff,
      durationMs >> 8,
      mask,
      stroke & 0xff,
      stroke >> 8,
      flags,
      ...squares,
    ]);
  }

  Future<LitchcraftV2State> addCopyFrameV2({
    required int session,
    required int baseRevision,
    required int source,
  }) => _frameEditV2(session, baseRevision, 0, source, 0, 0);

  Future<LitchcraftV2State> deleteFrameV2({
    required int session,
    required int baseRevision,
    required int source,
  }) => _frameEditV2(session, baseRevision, 1, source, 0, 0);

  Future<LitchcraftV2State> moveFrameV2({
    required int session,
    required int baseRevision,
    required int source,
    required int destination,
  }) => _frameEditV2(session, baseRevision, 2, source, destination, 0);

  Future<LitchcraftV2State> setFrameDurationV2({
    required int session,
    required int baseRevision,
    required int source,
    required int durationMs,
  }) => _frameEditV2(session, baseRevision, 3, source, 0, durationMs);

  Future<LitchcraftV2State> undoV2({required int session}) =>
      _v2State(LitchcraftV2Protocol.undo, session, const <int>[]);

  Future<LitchcraftV2State> redoV2({required int session}) =>
      _v2State(LitchcraftV2Protocol.redo, session, const <int>[]);

  Future<LitchcraftV2State> _timelineV2(
    LitchcraftV2TimelineAction action,
    int session,
    int frame,
  ) {
    _validateActiveSession(session);
    _validateFrame(frame, 'frame');
    return _v2State(LitchcraftV2Protocol.timeline, session, <int>[
      action.index,
      frame,
    ]);
  }

  Future<LitchcraftV2State> _frameEditV2(
    int session,
    int baseRevision,
    int action,
    int source,
    int destination,
    int durationMs,
  ) {
    _validateActiveSession(session);
    if (baseRevision < 0 || baseRevision > 0xffffffff) {
      throw ArgumentError.value(baseRevision, 'baseRevision');
    }
    _validateFrame(source, 'source');
    if ((action == 2 && (destination < 0 || destination >= 8)) ||
        (action != 2 && destination != 0) ||
        (action == 3 &&
            (durationMs < litchcraftAnimationMinimumFrameDurationMs ||
                durationMs > litchcraftAnimationMaximumFrameDurationMs)) ||
        (action != 3 && durationMs != 0)) {
      throw ArgumentError('Invalid v2 frame edit fields');
    }
    return _v2State(LitchcraftV2Protocol.frameEdit, session, <int>[
      baseRevision & 0xff,
      baseRevision >> 8 & 0xff,
      baseRevision >> 16 & 0xff,
      baseRevision >> 24,
      action,
      source,
      destination,
      durationMs & 0xff,
      durationMs >> 8,
    ]);
  }

  Future<LitchcraftV2State> _v2State(
    int operation,
    int session,
    List<int> payload,
  ) async {
    if (operation != LitchcraftV2Protocol.inspect) {
      _validateActiveSession(session);
    }
    final packet = await sendV2(operation, session, payload);
    return LitchcraftV2State.fromPacket(packet);
  }

  static void _validateActiveSession(int session) {
    if (session <= 0 || session > 0xffffffff) {
      throw ArgumentError.value(session, 'session');
    }
  }

  static void _validateFrame(int value, String name) {
    if (value < 0 || value >= 8) throw ArgumentError.value(value, name);
  }

  static void _validateEdit(
    int session,
    int frame,
    int stroke,
    int flags,
    int mask,
    List<int> squares,
  ) {
    _validateActiveSession(session);
    _validateFrame(frame, 'frame');
    if (stroke <= 0 ||
        stroke > 0xffff ||
        flags < 0 ||
        flags & ~3 != 0 ||
        mask < 1 ||
        mask > 15 ||
        squares.length > 3 ||
        squares.any((int square) => square < 0 || square >= 64)) {
      throw ArgumentError('Invalid v2 edit fields');
    }
  }

  Future<LitchcraftV2Packet> _sendV2(
    int operation,
    int session,
    List<int> payload,
    void Function(int)? onRequest, {
    _V2DocumentTransfer? owner,
    bool cleanup = false,
  }) async {
    await _waitForTool();
    if (_closed ||
        _v2RequestBusy ||
        _pending.values.any((p) => p.opcode != LitchcraftProtocol.tool) ||
        (_documentTransfer != null && !identical(_documentTransfer, owner))) {
      throw StateError('Litchcraft v2 link is unavailable or busy');
    }
    if (!cleanup) owner?.check();
    _nextId = (_nextId % 0xffff) + 1;
    final id = allocateRequestId?.call() ?? _nextId;
    final bytes = LitchcraftV2Protocol.request(operation, session, id, payload);
    final pending = _PendingV2Request(operation, session);
    _pendingV2[id] = pending;
    _v2RequestBusy = true;
    final outcome = pending.completer.future;
    unawaited(outcome.then<void>((_) {}, onError: (Object _, StackTrace _) {}));
    try {
      onRequest?.call(id);
      _diagnose('request_start', <String, Object?>{
        'protocol': 2,
        'operation': operation,
        'request_id': id,
        'session': session,
      });
      await Future<void>.value();
      for (var attempt = 0; attempt < 3; attempt++) {
        try {
          if (pending.cancelled) throw const LitchcraftTransferCancelled();
          if (_closed) throw StateError('Litchcraft link closed');
          if (!cleanup) owner?.check();
          final writer =
              operation == LitchcraftV2Protocol.streamCommit ||
                  operation == LitchcraftV2Protocol.streamCheckpoint
              ? writeBulkDocumentPacket!
              : write;
          await writer(bytes).timeout(timeout);
          final ack = await outcome.timeout(_ackWindow(attempt));
          if (ack.payload[0] != 0) throw LitchcraftRejected(ack.payload[0]);
          _diagnose('request_terminal', <String, Object?>{
            'protocol': 2,
            'operation': operation,
            'request_id': id,
            'session': ack.session,
            'revision': ack.revision,
            'attempt': attempt + 1,
            'outcome': 'acknowledged',
          });
          return ack;
        } on TimeoutException {
          if (pending.cancelled) throw const LitchcraftTransferCancelled();
          if (!cleanup && owner?.error != null) rethrow;
          if (_closed || attempt == 2) {
            final error = TimeoutException(
              'Litchcraft v2 notification acknowledgement timed out',
              timeout,
            );
            _fail(error);
            throw error;
          }
          _diagnose('request_retry', <String, Object?>{
            'protocol': 2,
            'operation': operation,
            'request_id': id,
            'session': session,
            'attempt': attempt + 2,
            'reason': 'ack_timeout',
          });
        }
      }
      throw StateError('Unreachable Litchcraft v2 retry');
    } catch (error) {
      _diagnose('request_terminal', <String, Object?>{
        'protocol': 2,
        'operation': operation,
        'request_id': id,
        'session': session,
        'outcome': error is LitchcraftRejected
            ? 'rejected'
            : error is TimeoutException
            ? 'timeout'
            : 'failed',
        if (error is LitchcraftRejected) 'status': error.status,
        'kind': error.runtimeType.toString(),
      });
      rethrow;
    } finally {
      _pendingV2.remove(id);
      pending.retireWrites();
      _v2RequestBusy = false;
      _startTimelineRecovery();
    }
  }

  Duration _ackWindow(int attempt) {
    if (attempt >= 2) return timeout;
    final candidate = attempt == 0 ? _firstAckWindow : _secondAckWindow;
    return timeout < candidate ? timeout : candidate;
  }

  Future<LitchcraftPacket> sendBulkReplace(
    int session,
    List<int> colors, [
    void Function(int)? onRequest,
  ]) async {
    final bulkWrite = writeBulkArtwork;
    if (_closed ||
        bulkWrite == null ||
        _v2RequestBusy ||
        _pending.isNotEmpty ||
        _documentTransfer != null) {
      throw StateError('Litchcraft bulk link unavailable');
    }
    _nextId = (_nextId % 0xffff) + 1;
    final id = allocateRequestId?.call() ?? _nextId;
    final pending = _PendingRequest(LitchcraftProtocol.bulkReplace, session);
    _pending[id] = pending;
    final outcome = pending.completer.future;
    unawaited(
      outcome.then<void>((_) {}, onError: (Object error, StackTrace stack) {}),
    );
    try {
      onRequest?.call(id);
      _diagnose('request_start', <String, Object?>{
        'opcode': LitchcraftProtocol.bulkReplace,
        'request_id': id,
        'session': session,
        'bulk': true,
      });
      for (var attempt = 0; attempt < 2; attempt++) {
        try {
          if (_closed || pending.cancelled) {
            throw const LitchcraftTransferCancelled();
          }
          await bulkWrite(session, id, colors).timeout(timeout);
          final ack = await outcome.timeout(timeout);
          if (ack.payload[0] != 0) throw LitchcraftRejected(ack.payload[0]);
          _diagnose('request_terminal', <String, Object?>{
            'opcode': LitchcraftProtocol.bulkReplace,
            'request_id': id,
            'session': ack.session,
            'revision': ack.revision,
            'attempt': attempt + 1,
            'outcome': 'acknowledged',
            'bulk': true,
          });
          return ack;
        } on TimeoutException {
          if (pending.cancelled) throw const LitchcraftTransferCancelled();
          if (_closed || attempt == 1) {
            final error = TimeoutException(
              'Litchcraft bulk acknowledgement timed out',
              timeout,
            );
            _fail(error);
            throw error;
          }
          _diagnose('request_retry', <String, Object?>{
            'opcode': LitchcraftProtocol.bulkReplace,
            'request_id': id,
            'session': session,
            'attempt': attempt + 2,
            'reason': 'ack_timeout',
            'bulk': true,
          });
        }
      }
      throw StateError('Unreachable Litchcraft bulk retry');
    } catch (error) {
      _diagnose('request_terminal', <String, Object?>{
        'opcode': LitchcraftProtocol.bulkReplace,
        'request_id': id,
        'session': session,
        'outcome': error is LitchcraftRejected
            ? 'rejected'
            : error is TimeoutException
            ? 'timeout'
            : 'failed',
        if (error is LitchcraftRejected) 'status': error.status,
        'kind': error.runtimeType.toString(),
        'bulk': true,
      });
      rethrow;
    } finally {
      _pending.remove(id);
      pending.settled.complete();
    }
  }

  void _fail(Object error) {
    if (_closed) return;
    _closed = true;
    for (final pending in _pending.values) {
      if (!pending.completer.isCompleted) {
        pending.completer.completeError(error);
      }
    }
    _pending.clear();
    for (final pending in _pendingV2.values) {
      pending.retireWrites();
      if (!pending.completer.isCompleted) {
        pending.completer.completeError(error);
      }
    }
    _pendingV2.clear();
    _timelineHalf?.timer?.cancel();
    _timelineHalf = null;
    _effectClockHalf = null;
    _effectClockRefreshRequest = null;
    _effectClock = null;
    _documentTransfer?.abort(error);
    onFailure(error);
  }

  Future<void> dispose() async {
    _fail(StateError('Litchcraft link closed'));
    await _subscription.cancel();
    await _timelineSamples.close();
  }
}

class LitchcraftV2Snapshot {
  const LitchcraftV2Snapshot({
    required this.session,
    required this.revision,
    required this.schema,
    required this.bytes,
    required this.crc32,
  });
  final int session, revision, schema, crc32;
  final Uint8List bytes;
}

class LitchcraftTransferCancelled implements Exception {
  const LitchcraftTransferCancelled();
}

/// One body and one inline descriptor, owned by the physical link. Deadlines
/// begin before the first request and only new accepted bytes refresh activity.
class _V2DocumentTransfer {
  _V2DocumentTransfer(
    this.link,
    this.inactivityTimeout,
    Duration absoluteTimeout,
    this.timer,
  ) {
    if (link._closed ||
        link._documentTransfer != null ||
        link._v2RequestBusy ||
        link._pending.isNotEmpty) {
      throw StateError('Litchcraft document transport is unavailable or busy');
    }
    link._documentTransfer = this;
    unawaited(_aborted.future.catchError((Object _) {}));
    _absolute = timer(absoluteTimeout, () {
      abort(TimeoutException('Litchcraft document absolute timeout'));
    });
    progress();
  }
  final LitchcraftLink link;
  final Duration inactivityTimeout;
  final Timer Function(Duration, void Function()) timer;
  final _aborted = Completer<void>();
  Timer? _inactivity, _absolute;
  Object? error;
  bool supersededByMode = false;
  void Function(LitchcraftV2Packet)? onPacket;

  void check() {
    final failure = error;
    if (failure != null) throw failure;
    if (link._closed) throw StateError('Litchcraft link closed');
  }

  void receive(LitchcraftV2Packet packet) {
    if (error == null) onPacket?.call(packet);
  }

  void progress() {
    _inactivity?.cancel();
    _inactivity = timer(inactivityTimeout, () {
      abort(TimeoutException('Litchcraft document inactivity timeout'));
    });
  }

  void abort(Object failure) {
    if (error != null) return;
    error = failure;
    _aborted.completeError(failure);
    for (final pending in link._pendingV2.values) {
      if (!pending.completer.isCompleted) {
        pending.completer.completeError(failure);
      }
    }
  }

  Future<T> wait<T>(Future<T> result) => Future.any<T>([
    result,
    _aborted.future.then<T>((_) => throw StateError('Transfer aborted')),
  ]);

  void finish() {
    stop();
    if (identical(link._documentTransfer, this)) link._documentTransfer = null;
  }

  void stop() {
    _inactivity?.cancel();
    _absolute?.cancel();
    onPacket = null;
  }
}

/// Bounded whole-document transaction owner. It is deliberately not connected
/// to capability discovery or presentation; a later controller owner may use
/// it after bit-14 admission without creating another notification stream.
class LitchcraftV2DocumentClient {
  LitchcraftV2DocumentClient(
    this._link, {
    Duration inactivityTimeout = const Duration(seconds: 6),
    Duration absoluteTimeout = const Duration(seconds: 180),
    Timer Function(Duration, void Function()) timer = Timer.new,
  }) : _inactivityTimeout = inactivityTimeout,
       _absoluteTimeout = absoluteTimeout,
       _timer = timer;
  final LitchcraftLink _link;
  final Duration _inactivityTimeout, _absoluteTimeout;
  final Timer Function(Duration, void Function()) _timer;
  _V2DocumentTransfer? _active;
  LitchcraftV2Snapshot? _lastSnapshot;
  LitchcraftV2Snapshot? get lastSnapshot => _lastSnapshot;
  bool get needsResync {
    final snapshot = _lastSnapshot;
    if (snapshot == null) return true;
    final watermark = _link.documentChangedRevision(snapshot.session);
    return _link._closed ||
        _link._v2Session != snapshot.session ||
        (watermark != null && _newer(watermark, snapshot.revision));
  }

  _V2DocumentTransfer _start() {
    final transfer = _V2DocumentTransfer(
      _link,
      _inactivityTimeout,
      _absoluteTimeout,
      _timer,
    );
    _active = transfer;
    return transfer;
  }

  void cancel() => _active?.abort(const LitchcraftTransferCancelled());

  void _finish(_V2DocumentTransfer transfer) {
    transfer.finish();
    if (identical(_active, transfer)) _active = null;
  }

  static bool _newer(int candidate, int baseline) {
    final delta = (candidate - baseline) & 0xffffffff;
    return delta != 0 && delta < 0x80000000;
  }

  Future<LitchcraftV2Snapshot> snapshot({
    int session = 0,
    int maximumPacketBytes = LitchcraftV2Protocol.minimumPacketBytes,
    void Function(int confirmed, int total)? onProgress,
  }) async {
    _validatePacketBytes(maximumPacketBytes);
    final transfer = _start();
    int snapshotRequest = 0, snapshotSession = session;
    final startedAt = _link._monotonicNow();
    var chunks = 0, attempts = 0, barriers = 0, receivedBytes = 0;
    var coherent = false;
    try {
      // One coalesced recovery operation, capped at the established three
      // snapshot attempts; transport recovery stays owned by the controller.
      for (var attempt = 0; attempt < 3; attempt++) {
        attempts++;
        final body = Completer<LitchcraftV2Snapshot>();
        unawaited(body.future.then<void>((_) {}, onError: (Object _) {}));
        int request = 0, offset = 0, crc = 0;
        LitchcraftV2Packet? state;
        Uint8List? bytes;
        transfer.onPacket = (packet) {
          if (packet.event == LitchcraftV2Protocol.state &&
              packet.requestId == 0) {
            if (state != null &&
                (packet.session != state!.session ||
                    packet.payload[1] & 1 == 0)) {
              transfer.abort(StateError('Litchcraft session ended'));
            }
            return;
          }
          if (packet.requestId != request || body.isCompleted) return;
          if (packet.event == LitchcraftV2Protocol.state) {
            if (packet.payload[0] == 0) {
              if (packet.payload[4] != LitchcraftV2Protocol.schema ||
                  packet.payload[2] == 0) {
                transfer.abort(
                  const FormatException('Snapshot has no document'),
                );
                return;
              }
              state = packet;
            }
            return;
          }
          final ack = state;
          if (ack == null) {
            transfer.abort(const FormatException('Snapshot before State ACK'));
            return;
          }
          if (packet.session != ack.session ||
              packet.revision != ack.revision) {
            return;
          }
          try {
            if (packet.event == LitchcraftV2Protocol.snapshotBegin) {
              if (bytes != null) {
                throw const FormatException('Duplicate snapshot begin');
              }
              final length = packet.payload[1] | packet.payload[2] << 8;
              if (length != 32 + ack.payload[2] * 1538 ||
                  packet.payload[0] != ack.payload[4]) {
                throw const FormatException('Snapshot metadata mismatch');
              }
              crc = ByteData.sublistView(
                Uint8List.fromList(packet.payload),
              ).getUint32(3, Endian.little);
              bytes = Uint8List(length);
              onProgress?.call(0, length);
            } else if (packet.event == LitchcraftV2Protocol.snapshotChunk) {
              final target = bytes;
              if (target == null) {
                throw const FormatException('Chunk before begin');
              }
              final start = packet.payload[0] | packet.payload[1] << 8;
              final count = packet.payload.length - 2;
              if (packet.payload.length + 13 > maximumPacketBytes ||
                  start != offset ||
                  offset + count > target.length) {
                throw const FormatException('Invalid snapshot coverage');
              }
              target.setRange(offset, offset + count, packet.payload, 2);
              offset += count;
              chunks++;
              receivedBytes += count;
              transfer.progress();
              onProgress?.call(offset, target.length);
            } else if (packet.event == LitchcraftV2Protocol.snapshotEnd) {
              final target = bytes;
              if (target == null || offset != target.length) {
                throw const FormatException('Incomplete snapshot');
              }
              decodeLitchcraftAnimationDocument(target, crc);
              body.complete(
                LitchcraftV2Snapshot(
                  session: ack.session,
                  revision: ack.revision,
                  schema: ack.payload[4],
                  bytes: target.asUnmodifiableView(),
                  crc32: crc,
                ),
              );
            }
          } on Object catch (error) {
            transfer.abort(error);
          }
        };
        await _link._sendV2(LitchcraftV2Protocol.snapshot, session, const [], (
          id,
        ) {
          request = id;
          snapshotRequest = id;
          snapshotSession = session;
        }, owner: transfer);
        final snapshot = await transfer.wait(body.future);
        transfer.check();
        session = snapshot.session;
        // End only validates the frozen body. The following round trip is an
        // ordering barrier for a Changed queued on a later notification turn.
        barriers++;
        final current = await _link._sendV2(
          LitchcraftV2Protocol.inspect,
          session,
          const [],
          null,
          owner: transfer,
        );
        transfer.check();
        final watermark = _link.documentChangedRevision(session);
        if (current.session == snapshot.session &&
            current.revision == snapshot.revision &&
            current.payload[4] == snapshot.schema &&
            current.payload[2] == snapshot.bytes[1] &&
            (watermark == null || !_newer(watermark, snapshot.revision))) {
          _lastSnapshot = snapshot;
          coherent = true;
          return snapshot;
        }
      }
      throw const LitchcraftResyncRequired();
    } finally {
      _link._diagnose('document_snapshot_complete', {
        'elapsed_ms': _link._monotonicNow() - startedAt,
        'bytes': receivedBytes,
        'chunks': chunks,
        'attempts': attempts,
        'inspect_barriers': barriers,
        'maximum_payload': maximumPacketBytes,
        'coherent': coherent,
      });
      transfer.stop();
      if (!coherent && snapshotRequest != 0 && !_link._closed) {
        await _cancelTransfer(transfer, snapshotSession, snapshotRequest);
      }
      _finish(transfer);
    }
  }

  Future<LitchcraftV2Snapshot> inspectThenSnapshot() async {
    final ack = await _link.sendV2(LitchcraftV2Protocol.inspect, 0);
    final snapshot = await this.snapshot(session: ack.session);
    if (snapshot.session != ack.session && ack.session != 0) {
      throw const LitchcraftResyncRequired();
    }
    return snapshot;
  }

  Future<LitchcraftV2Packet> replace({
    required int session,
    required int baseRevision,
    required List<int> document,
    required int crc32,
    bool inactiveStorage = false,
    int maximumPacketBytes = LitchcraftV2Protocol.minimumPacketBytes,
    void Function(int confirmed, int total)? onProgress,
  }) async {
    if (inactiveStorage != (session == 0)) {
      throw ArgumentError(
        'Inactive storage replacement must use the zero session',
      );
    }
    _validatePacketBytes(maximumPacketBytes);
    // Validate before reserving any transport/staging, then freeze caller bytes.
    decodeLitchcraftAnimationDocument(document, crc32);
    final beginPayload = LitchcraftV2Protocol.replaceBegin(
      session: session,
      request: 1,
      baseRevision: baseRevision,
      length: document.length,
      crc32: crc32,
    ).sublist(9);
    final bytes = Uint8List.fromList(document);
    final transfer = _start();
    var transferId = 0;
    var committed = false;
    try {
      transfer.onPacket = (packet) {
        if (packet.event == LitchcraftV2Protocol.changed &&
            packet.requestId == 0 &&
            packet.session == session &&
            _newer(packet.revision, baseRevision)) {
          transfer.abort(const LitchcraftRejected(9));
        } else if (!inactiveStorage &&
            packet.event == LitchcraftV2Protocol.state &&
            packet.requestId == 0 &&
            (packet.session != session || packet.payload[1] & 1 == 0)) {
          transfer.abort(StateError('Litchcraft session ended'));
        }
      };
      final needsWindows =
          maximumPacketBytes < LitchcraftV2Protocol.maximumPacketBytes;
      var streamed =
          _link.writeBulkDocumentPacket != null &&
          _link._bulkDocumentSupported != false &&
          (!needsWindows || _link._bulkDocumentWindowSupported != false);
      if (streamed &&
          (_link._bulkDocumentSupported == null ||
              needsWindows && _link._bulkDocumentWindowSupported == null)) {
        // Unknown mutations on an inactive old peer fail StaleSession before
        // opcode validation. A metadata probe is safe in both ownership states.
        try {
          await _link._sendV2(
            LitchcraftV2Protocol.inspect,
            session,
            [needsWindows ? 3 : 2],
            null,
            owner: transfer,
          );
          _link._bulkDocumentSupported = true;
          if (needsWindows) _link._bulkDocumentWindowSupported = true;
        } on LitchcraftRejected catch (error) {
          if (error.status != 3 && error.status != 4) rethrow;
          if (needsWindows) {
            _link._bulkDocumentWindowSupported = false;
          } else {
            _link._bulkDocumentSupported = false;
          }
          streamed = false;
        }
      }
      Future<LitchcraftV2Packet> beginTransfer() => _link._sendV2(
        streamed
            ? LitchcraftV2Protocol.streamBegin
            : LitchcraftV2Protocol.replaceBeginOp,
        session,
        beginPayload,
        (id) => transferId = id,
        owner: transfer,
      );
      late LitchcraftV2Packet begin;
      try {
        begin = await beginTransfer();
        if (streamed) _link._bulkDocumentSupported = true;
      } on LitchcraftRejected catch (error) {
        if (!streamed || (error.status != 3 && error.status != 4)) rethrow;
        streamed = false;
        _link._bulkDocumentSupported = false;
        begin = await beginTransfer();
      }
      if (begin.revision != baseRevision) {
        throw const LitchcraftRejected(9);
      }
      onProgress?.call(0, bytes.length);
      final perChunk = maximumPacketBytes - 13;
      var windowPackets = 0;
      for (var offset = 0; offset < bytes.length; offset += perChunk) {
        final end = (offset + perChunk).clamp(0, bytes.length);
        final payload = <int>[
          transferId & 0xff,
          transferId >> 8,
          offset & 0xff,
          offset >> 8,
          ...bytes.sublist(offset, end),
        ];
        if (streamed) {
          await _link._waitForTool();
          transfer.check();
          final packet = LitchcraftV2Protocol.request(
            LitchcraftV2Protocol.streamChunk,
            session,
            transferId,
            payload,
          );
          await _link.writeBulkDocumentPacket!(packet).timeout(_link.timeout);
          // At most 32 data packets plus this receipt occupy the 64-slot
          // firmware FIFO. Its cursor ACK proves the preceding window drained;
          // native write completion alone only proves radio-stack admission.
          if (needsWindows && ++windowPackets == 32 && end < bytes.length) {
            final ack = await _link._sendV2(
              LitchcraftV2Protocol.streamCheckpoint,
              session,
              [transferId & 0xff, transferId >> 8, end & 0xff, end >> 8],
              null,
              owner: transfer,
            );
            if (ack.revision != baseRevision) {
              throw const LitchcraftRejected(9);
            }
            windowPackets = 0;
          }
        } else {
          final ack = await _link._sendV2(
            LitchcraftV2Protocol.replaceChunkOp,
            session,
            payload,
            null,
            owner: transfer,
          );
          if (ack.revision != baseRevision) throw const LitchcraftRejected(9);
        }
        transfer.check();
        transfer.progress();
        onProgress?.call(end, bytes.length);
      }
      // The commit itself can emit Changed after its ACK. It is the operation's
      // new authority, not a competing physical mutation.
      transfer.onPacket = null;
      final commit = await _link._sendV2(
        streamed
            ? LitchcraftV2Protocol.streamCommit
            : LitchcraftV2Protocol.replaceCommitOp,
        session,
        <int>[transferId & 0xff, transferId >> 8],
        null,
        owner: transfer,
      );
      transfer.check();
      if (commit.revision != baseRevision &&
          commit.revision != ((baseRevision + 1) & 0xffffffff)) {
        throw const FormatException('Invalid replacement revision');
      }
      committed = true;
      return commit;
    } finally {
      // Cancel only this link's staging. A closed old link cannot send cleanup
      // through the newly connected device even if its native write settles late.
      transfer.stop();
      if (!committed && transferId != 0 && !_link._closed) {
        await _cancelTransfer(transfer, session, transferId);
      }
      _finish(transfer);
    }
  }

  Future<void> _cancelTransfer(
    _V2DocumentTransfer owner,
    int session,
    int id,
  ) async {
    if (owner.supersededByMode) return;
    try {
      await _link._sendV2(
        LitchcraftV2Protocol.cancelTransfer,
        session,
        LitchcraftProtocol.u16(id),
        null,
        owner: owner,
        cleanup: true,
      );
    } catch (_) {}
  }

  static void _validatePacketBytes(int value) {
    if (value < LitchcraftV2Protocol.minimumPacketBytes ||
        value > LitchcraftV2Protocol.maximumPacketBytes) {
      throw ArgumentError.value(value, 'maximumPacketBytes');
    }
  }
}
