import 'dart:async';
import 'dart:typed_data';

import 'package:flutter/foundation.dart' show listEquals;

import '../models/litchcraft_animation.dart';
import '../models/litchcraft_animation_codec.dart';
import '../models/litchcraft_state.dart';
import '../models/litchcraft_wire_codec.dart';
import 'app_trace_recorder.dart';
import 'litchcraft_protocol.dart';

bool _isNewerFrameStoreGeneration(int candidate, int current) {
  final delta = (candidate - current) & 0xffffffff;
  return delta != 0 && delta < 0x80000000;
}

/// The board accepted Commit but has not yet published its durable root.
/// The caller must retain its proposed artwork until same-session authority
/// can be read back; Cancel cannot restore the previous document here.
class LitchcraftFrameStoreCommitPending implements Exception {
  const LitchcraftFrameStoreCommitPending(this.completion, this.cancel);

  final Future<LitchcraftFrameStoreTransferResult> completion;
  final Future<void> Function() cancel;
}

/// Selects the active owner for a schema-4 transfer. An empty frame store has
/// no document session yet, so its first transfer is owned by the already
/// admitted Litchcraft mode session.
int litchcraftFrameStoreAuthoritySession({
  required int documentSession,
  required int modeSession,
}) => documentSession != 0 ? documentSession : modeSession;

/// Schema-4 Store transport over the shared connection session.
class LitchcraftFrameStoreProtocol {
  static const int keyframeInterval = 32;
  static const int schema = 4;
  static const int residentListingCapability = 7;
  static const int storeBegin = LitchcraftV2Protocol.frameStoreBegin;
  static const int storeChunk = LitchcraftV2Protocol.frameStoreChunk;
  static const int storeCheckpoint = LitchcraftV2Protocol.frameStoreCheckpoint;
  static const int storeCommit = LitchcraftV2Protocol.frameStoreCommit;
  static const int storeCancel = LitchcraftV2Protocol.frameStoreCancel;
  static const int storeState = LitchcraftV2Protocol.frameStoreState;
  static const int storeSnapshot = LitchcraftV2Protocol.frameStoreSnapshot;
  static const int storeSnapshotChunk =
      LitchcraftV2Protocol.frameStoreSnapshotChunk;
  static const int storeResidents = LitchcraftV2Protocol.frameStoreResidents;
  static const int storeSelect = LitchcraftV2Protocol.frameStoreSelect;
  static const int storeResidentsState =
      LitchcraftV2Protocol.frameStoreResidentsState;

  static const int _headerBytes = 9;
  static const int _frameRecordHeaderBytes = 6;
  static const int _beginPayloadBytes = 44;

  static Uint8List capabilityProbe({
    required int session,
    required int request,
  }) => LitchcraftV2Protocol.request(
    LitchcraftV2Protocol.inspect,
    session,
    request,
    <int>[residentListingCapability],
  );

  static Uint8List snapshotRequest({
    required int session,
    required int request,
    required int generation,
    required int cursor,
    int? frame,
    int? liveRevision,
  }) {
    if (session <= 0 ||
        session > 0xffffffff ||
        request <= 0 ||
        request > 0xffff ||
        generation <= 0 ||
        generation > 0xffffffff ||
        cursor < 0 ||
        cursor > 0xffffffff ||
        (frame != null &&
            frame != 0xffff &&
            !(liveRevision != null && frame == 0xfffe) &&
            (frame < 0 || frame > 255)) ||
        (liveRevision != null &&
            (frame == null ||
                liveRevision <= 0 ||
                liveRevision > 0xffffffff)) ||
        (frame == 0xffff && cursor != 0)) {
      throw ArgumentError('Invalid Litchcraft frame-store snapshot request');
    }
    final packet = _packet(
      storeSnapshot,
      session,
      request,
      liveRevision != null
          ? 14
          : frame == null
          ? 8
          : 10,
    );
    final data = ByteData.sublistView(packet);
    data.setUint32(9, generation, Endian.little);
    data.setUint32(13, cursor, Endian.little);
    if (frame != null) data.setUint16(17, frame, Endian.little);
    if (liveRevision != null) {
      data.setUint32(19, liveRevision, Endian.little);
    }
    return packet;
  }

  static Uint8List residentsRequest({
    required int session,
    required int request,
  }) => _packet(storeResidents, session, request, 0);

  static Uint8List selectResidentRequest({
    required int session,
    required int request,
    required LitchcraftFrameStoreResident resident,
  }) {
    final packet = _packet(storeSelect, session, request, 9);
    final data = ByteData.sublistView(packet);
    packet[9] = resident.rootSlot;
    data.setUint32(10, resident.generation, Endian.little);
    data.setUint32(14, resident.bodyCrc32, Endian.little);
    return packet;
  }

  static LitchcraftFrameStoreState? decodeState(
    List<int> bytes, {
    int? expectedSession,
    int? expectedRequest,
  }) {
    final packet = LitchcraftV2Packet.decode(bytes);
    if (packet == null || packet.event != storeState) return null;
    if (expectedSession != null && packet.session != expectedSession) {
      return null;
    }
    if (expectedRequest != null && packet.requestId != expectedRequest) {
      return null;
    }
    return LitchcraftFrameStoreState.fromPacket(packet);
  }

  static LitchcraftFrameStoreTransfer prepare({
    required int session,
    required int transfer,
    required int expectedGeneration,
    bool enableRgbWire = false,
    required int topology,
    required bool active,
    required bool playbackOnEntry,
    required int sparkleSeed,
    required List<int> favoriteColors,
    required List<LitchcraftAnimationFrame> frames,
  }) {
    if (session < 1 ||
        session > 0xffffffff ||
        transfer < 1 ||
        transfer > 0xffff ||
        expectedGeneration < 0 ||
        expectedGeneration > 0xffffffff ||
        topology < 1 ||
        topology > 3 ||
        (playbackOnEntry && !active) ||
        sparkleSeed < 1 ||
        sparkleSeed > 0xffffffff ||
        favoriteColors.length != 8 ||
        favoriteColors.any((color) => color < 0 || color > 0xffffff) ||
        frames.isEmpty ||
        frames.length > 256) {
      throw ArgumentError('Invalid Litchcraft schema-4 transfer');
    }

    final records = <Uint8List>[];
    LitchcraftAnimationFrame? previous;
    var bodyBytes = 0;
    var bodyCrcState = 0xffffffff;
    var canonicalBodyBytes = 0;
    var canonicalBodyCrcState = 0xffffffff;
    for (var index = 0; index < frames.length; index++) {
      final frame = frames[index];
      final predecessor = index % keyframeInterval == 0 ? null : previous;
      final encoded = encodeLitchcraftWireFrame(
        previous: predecessor,
        frame: frame,
        enableRgbOnly: enableRgbWire,
      );
      final record = _frameRecord(encoded);
      records.add(record);
      bodyBytes += record.length;
      bodyCrcState = _updateCrc32(bodyCrcState, record);
      if (enableRgbWire) {
        final canonicalRecord = _frameRecord(
          encodeLitchcraftWireFrame(previous: predecessor, frame: frame),
        );
        canonicalBodyBytes += canonicalRecord.length;
        canonicalBodyCrcState = _updateCrc32(
          canonicalBodyCrcState,
          canonicalRecord,
        );
      }
      previous = frame;
    }
    return LitchcraftFrameStoreTransfer._(
      session: session,
      transfer: transfer,
      expectedGeneration: expectedGeneration,
      topology: topology,
      active: active,
      playbackOnEntry: playbackOnEntry,
      sparkleSeed: sparkleSeed,
      favoriteColors: List<int>.unmodifiable(favoriteColors),
      records: records,
      enableRgbWire: enableRgbWire,
      bodyBytes: bodyBytes,
      bodyCrc32: (~bodyCrcState) & 0xffffffff,
      canonicalBodyBytes: enableRgbWire ? canonicalBodyBytes : bodyBytes,
      canonicalBodyCrc32: enableRgbWire
          ? (~canonicalBodyCrcState) & 0xffffffff
          : (~bodyCrcState) & 0xffffffff,
    );
  }

  /// Canonical resident identity without retaining a transfer body or binding
  /// it to a request, mode session, or durable generation.
  static LitchcraftFrameStoreContentIdentity identify(
    LitchcraftAnimationDocument document,
  ) {
    var bodyBytes = 0;
    var crcState = 0xffffffff;
    LitchcraftAnimationFrame? previous;
    for (var index = 0; index < document.frames.length; index++) {
      final frame = document.frames[index];
      final record = _frameRecord(
        encodeLitchcraftWireFrame(
          previous: index % keyframeInterval == 0 ? null : previous,
          frame: frame,
        ),
      );
      bodyBytes += record.length;
      crcState = _updateCrc32(crcState, record);
      previous = frame;
    }
    return LitchcraftFrameStoreContentIdentity(
      bodyBytes: bodyBytes,
      bodyCrc32: (~crcState) & 0xffffffff,
      frameCount: document.frames.length,
      topology: document.topology.id,
      playbackOnEntry: document.playbackOnEntry,
      sparkleSeed: document.sparkleSeed,
    );
  }

  static Uint8List _frameRecord(Uint8List encoded) {
    final record = Uint8List(_frameRecordHeaderBytes + encoded.length);
    final data = ByteData.sublistView(record);
    data.setUint16(0, encoded.length, Endian.little);
    data.setUint32(2, litchcraftAnimationCrc32(encoded), Endian.little);
    record.setRange(_frameRecordHeaderBytes, record.length, encoded);
    return record;
  }

  static Uint8List _packet(
    int operation,
    int session,
    int transfer,
    int payloadBytes,
  ) {
    final packet = Uint8List(_headerBytes + payloadBytes);
    final data = ByteData.sublistView(packet);
    packet[0] = LitchcraftV2Protocol.magic;
    packet[1] = LitchcraftV2Protocol.version;
    packet[2] = operation;
    data.setUint32(3, session, Endian.little);
    data.setUint16(7, transfer, Endian.little);
    return packet;
  }

  static int _updateCrc32(int state, List<int> bytes) {
    var next = state;
    for (final byte in bytes) {
      next ^= byte;
      for (var bit = 0; bit < 8; bit++) {
        next = (next >>> 1) ^ ((next & 1) == 0 ? 0 : 0xedb88320);
      }
    }
    return next;
  }
}

/// Pure schema-4 artwork identity. Palette and mode/runtime state are separate
/// from canonical frame bytes; board authority must still be freshly checked.
class LitchcraftFrameStoreContentIdentity {
  const LitchcraftFrameStoreContentIdentity({
    required this.bodyBytes,
    required this.bodyCrc32,
    required this.frameCount,
    required this.topology,
    required this.playbackOnEntry,
    required this.sparkleSeed,
  });

  final int bodyBytes, bodyCrc32, frameCount, topology, sparkleSeed;
  final bool playbackOnEntry;
}

class LitchcraftFrameStoreTransfer {
  LitchcraftFrameStoreTransfer._({
    required this.session,
    required this.transfer,
    required this.expectedGeneration,
    required this.topology,
    required this.active,
    required this.playbackOnEntry,
    required this.sparkleSeed,
    required this.favoriteColors,
    required List<Uint8List> records,
    required this.enableRgbWire,
    required this.bodyBytes,
    required this.bodyCrc32,
    required this.canonicalBodyBytes,
    required this.canonicalBodyCrc32,
  }) : _records = records
           .map((record) => Uint8List.fromList(record))
           .toList(growable: false);

  final int session;
  final int transfer;
  final int expectedGeneration;
  final int topology;
  final bool active;
  final bool playbackOnEntry;
  final int sparkleSeed;
  final List<int> favoriteColors;
  final List<Uint8List> _records;
  final bool enableRgbWire;

  /// Incoming transfer body length and CRC used by Begin and Commit.
  final int bodyBytes;
  final int bodyCrc32;

  /// Legacy schema-4 body identity retained by resident roots.
  final int canonicalBodyBytes;
  final int canonicalBodyCrc32;

  int get frameCount => _records.length;

  LitchcraftFrameStoreContentIdentity get contentIdentity =>
      LitchcraftFrameStoreContentIdentity(
        bodyBytes: canonicalBodyBytes,
        bodyCrc32: canonicalBodyCrc32,
        frameCount: frameCount,
        topology: topology,
        playbackOnEntry: playbackOnEntry,
        sparkleSeed: sparkleSeed,
      );

  Uint8List beginPacket() {
    final packet = LitchcraftFrameStoreProtocol._packet(
      LitchcraftFrameStoreProtocol.storeBegin,
      session,
      transfer,
      LitchcraftFrameStoreProtocol._beginPayloadBytes,
    );
    final data = ByteData.sublistView(packet);
    var offset = 9;
    data.setUint32(offset, expectedGeneration, Endian.little);
    data.setUint32(offset += 4, bodyBytes, Endian.little);
    data.setUint32(offset += 4, bodyCrc32, Endian.little);
    data.setUint16(offset += 4, frameCount, Endian.little);
    packet[offset += 2] = topology;
    // RGB tags are opt-in; independent raw anchors still reset the chain.
    packet[++offset] =
        (active ? 1 : 0) |
        (playbackOnEntry ? 2 : 0) |
        4 |
        (enableRgbWire ? 8 : 0);
    data.setUint32(++offset, sparkleSeed, Endian.little);
    offset += 4;
    for (final color in favoriteColors) {
      packet[offset++] = color >> 16;
      packet[offset++] = color >> 8;
      packet[offset++] = color;
    }
    return packet;
  }

  Iterable<Uint8List> chunkPackets({required int maximumPacketBytes}) sync* {
    if (maximumPacketBytes < 20 || maximumPacketBytes > 512) {
      throw ArgumentError.value(maximumPacketBytes, 'maximumPacketBytes');
    }
    final payloadCapacity = maximumPacketBytes - 13;
    var cursor = 0;
    var recordIndex = 0;
    var recordOffset = 0;
    while (cursor < bodyBytes) {
      final packet = LitchcraftFrameStoreProtocol._packet(
        LitchcraftFrameStoreProtocol.storeChunk,
        session,
        transfer,
        (bodyBytes - cursor).clamp(0, payloadCapacity) + 4,
      );
      final data = ByteData.sublistView(packet);
      data.setUint32(9, cursor, Endian.little);
      var destination = 13;
      while (destination < packet.length) {
        final record = _records[recordIndex];
        final count = (packet.length - destination).clamp(
          0,
          record.length - recordOffset,
        );
        packet.setRange(destination, destination + count, record, recordOffset);
        destination += count;
        recordOffset += count;
        cursor += count;
        if (recordOffset == record.length) {
          recordIndex++;
          recordOffset = 0;
        }
      }
      yield packet;
    }
  }

  Uint8List checkpointPacket({required int completedBodyBytes}) {
    if (completedBodyBytes < 0 || completedBodyBytes > bodyBytes) {
      throw ArgumentError.value(completedBodyBytes, 'completedBodyBytes');
    }
    final packet = LitchcraftFrameStoreProtocol._packet(
      LitchcraftFrameStoreProtocol.storeCheckpoint,
      session,
      transfer,
      4,
    );
    ByteData.sublistView(
      packet,
    ).setUint32(9, completedBodyBytes, Endian.little);
    return packet;
  }

  Uint8List commitPacket() {
    final packet = LitchcraftFrameStoreProtocol._packet(
      LitchcraftFrameStoreProtocol.storeCommit,
      session,
      transfer,
      4,
    );
    ByteData.sublistView(packet).setUint32(9, bodyBytes, Endian.little);
    return packet;
  }

  Uint8List cancelPacket() => LitchcraftFrameStoreProtocol._packet(
    LitchcraftFrameStoreProtocol.storeCancel,
    session,
    transfer,
    0,
  );
}

/// The schema-4 capability and committed-root response. The common v2 header's
/// revision field is the committed frame-store generation.
class LitchcraftFrameStoreState {
  const LitchcraftFrameStoreState({
    required this.session,
    required this.generation,
    required this.requestId,
    required this.status,
    required this.active,
    required this.playing,
    required this.hasArtwork,
    required this.frameCount,
    required this.selectedFrame,
    required this.topology,
    this.rgbWireCapable = false,
    this.selectedRootSlot,
    this.residents = const <LitchcraftFrameStoreResident>[],
    this.includesResidentListing = false,
  });

  final int session;
  final int generation;
  final int requestId;
  final int status;
  final bool active;
  final bool playing;
  final bool hasArtwork;
  final int frameCount;
  final int selectedFrame;
  final int topology;
  final bool rgbWireCapable;
  final int? selectedRootSlot;
  final List<LitchcraftFrameStoreResident> residents;
  final bool includesResidentListing;

  LitchcraftFrameStoreResidents? get residentListing => includesResidentListing
      ? LitchcraftFrameStoreResidents(
          session: session,
          generation: generation,
          requestId: requestId,
          status: status,
          selectedRootSlot: selectedRootSlot,
          residents: residents,
        )
      : null;

  factory LitchcraftFrameStoreState.fromPacket(LitchcraftV2Packet packet) {
    if (packet.event != LitchcraftFrameStoreProtocol.storeState) {
      throw ArgumentError.value(packet, 'packet', 'Expected Store state');
    }
    final payload = packet.payload;
    if (payload.length < 6) {
      throw const FormatException('Truncated frame-store state');
    }
    int? selectedRootSlot;
    final residents = <LitchcraftFrameStoreResident>[];
    var includesResidentListing = false;
    if (payload.length > 6) {
      if (payload.length < 8) {
        throw const FormatException('Truncated frame-store resident listing');
      }
      final count = payload[7];
      if (count > 2 || payload.length != 8 + count * 45) {
        throw const FormatException('Malformed frame-store resident listing');
      }
      includesResidentListing = true;
      selectedRootSlot = payload[6] == 0xff ? null : payload[6];
      final data = ByteData.sublistView(Uint8List.fromList(payload));
      for (var index = 0; index < count; index++) {
        final offset = 8 + index * 45;
        final flags = payload[offset + 16];
        final colors = <int>[
          for (
            var colorOffset = offset + 21;
            colorOffset < offset + 45;
            colorOffset += 3
          )
            payload[colorOffset] << 16 |
                payload[colorOffset + 1] << 8 |
                payload[colorOffset + 2],
        ];
        residents.add(
          LitchcraftFrameStoreResident(
            rootSlot: payload[offset],
            generation: data.getUint32(offset + 1, Endian.little),
            bodyCrc32: data.getUint32(offset + 5, Endian.little),
            bodyBytes: data.getUint32(offset + 9, Endian.little),
            frameCount: data.getUint16(offset + 13, Endian.little),
            topology: payload[offset + 15],
            active: flags & 1 != 0,
            playbackOnEntry: flags & 2 != 0,
            predecessorDeltas: flags & 4 != 0,
            sparkleSeed: data.getUint32(offset + 17, Endian.little),
            favoriteColors: List<int>.unmodifiable(colors),
          ),
        );
      }
    }
    return LitchcraftFrameStoreState(
      session: packet.session,
      generation: packet.revision,
      requestId: packet.requestId,
      status: payload[0],
      active: payload[1] & 1 != 0,
      playing: payload[1] & 2 != 0,
      hasArtwork: payload[1] & 4 != 0,
      frameCount: payload[2] | payload[3] << 8,
      selectedFrame: payload[4] | payload[5] << 8,
      topology: ((payload[1] >> 3) & 3) + 1,
      rgbWireCapable: payload[1] & 0x20 != 0,
      selectedRootSlot: selectedRootSlot,
      residents: List<LitchcraftFrameStoreResident>.unmodifiable(residents),
      includesResidentListing: includesResidentListing,
    );
  }
}

class LitchcraftFrameStoreResident {
  const LitchcraftFrameStoreResident({
    required this.rootSlot,
    required this.generation,
    required this.bodyCrc32,
    required this.bodyBytes,
    required this.frameCount,
    required this.topology,
    required this.active,
    required this.playbackOnEntry,
    required this.predecessorDeltas,
    required this.sparkleSeed,
    required this.favoriteColors,
  });

  final int rootSlot;
  final int generation;
  final int bodyCrc32;
  final int bodyBytes;
  final int frameCount;
  final int topology;
  final bool active;
  final bool playbackOnEntry;
  final bool predecessorDeltas;
  final int sparkleSeed;
  final List<int> favoriteColors;

  bool matchesArtworkExceptFavorites(LitchcraftFrameStoreTransfer transfer) =>
      matchesContent(transfer.contentIdentity);

  bool matchesContent(LitchcraftFrameStoreContentIdentity identity) =>
      bodyCrc32 == identity.bodyCrc32 &&
      bodyBytes == identity.bodyBytes &&
      frameCount == identity.frameCount &&
      topology == identity.topology &&
      active &&
      playbackOnEntry == identity.playbackOnEntry &&
      predecessorDeltas &&
      sparkleSeed == identity.sparkleSeed;

  bool matches(LitchcraftFrameStoreTransfer transfer) =>
      matchesArtworkExceptFavorites(transfer) &&
      listEquals(favoriteColors, transfer.favoriteColors);
}

class LitchcraftFrameStoreResidents {
  const LitchcraftFrameStoreResidents({
    required this.session,
    required this.generation,
    required this.requestId,
    required this.status,
    required this.selectedRootSlot,
    required this.residents,
  });

  final int session;
  final int generation;
  final int requestId;
  final int status;
  final int? selectedRootSlot;
  final List<LitchcraftFrameStoreResident> residents;

  factory LitchcraftFrameStoreResidents.fromPacket(LitchcraftV2Packet packet) {
    if (packet.event != LitchcraftFrameStoreProtocol.storeResidentsState) {
      throw ArgumentError.value(packet, 'packet', 'Expected Store residents');
    }
    final payload = Uint8List.fromList(packet.payload);
    final data = ByteData.sublistView(payload);
    final residents = <LitchcraftFrameStoreResident>[];
    for (var index = 0; index < payload[2]; index++) {
      final offset = 3 + index * 45;
      final flags = payload[offset + 16];
      final colors = <int>[
        for (
          var colorOffset = offset + 21;
          colorOffset < offset + 45;
          colorOffset += 3
        )
          payload[colorOffset] << 16 |
              payload[colorOffset + 1] << 8 |
              payload[colorOffset + 2],
      ];
      residents.add(
        LitchcraftFrameStoreResident(
          rootSlot: payload[offset],
          generation: data.getUint32(offset + 1, Endian.little),
          bodyCrc32: data.getUint32(offset + 5, Endian.little),
          bodyBytes: data.getUint32(offset + 9, Endian.little),
          frameCount: data.getUint16(offset + 13, Endian.little),
          topology: payload[offset + 15],
          active: flags & 1 != 0,
          playbackOnEntry: flags & 2 != 0,
          predecessorDeltas: flags & 4 != 0,
          sparkleSeed: data.getUint32(offset + 17, Endian.little),
          favoriteColors: List<int>.unmodifiable(colors),
        ),
      );
    }
    return LitchcraftFrameStoreResidents(
      session: packet.session,
      generation: packet.revision,
      requestId: packet.requestId,
      status: payload[0],
      selectedRootSlot: payload[1] == 0xff ? null : payload[1],
      residents: List<LitchcraftFrameStoreResident>.unmodifiable(residents),
    );
  }
}

class LitchcraftFrameStoreSnapshotChunk {
  const LitchcraftFrameStoreSnapshotChunk({
    required this.session,
    required this.generation,
    required this.requestId,
    required this.status,
    required this.active,
    required this.playing,
    required this.predecessorDeltas,
    this.singleFrame = false,
    this.liveRevision = 0,
    this.documentEpoch = 0,
    this.coveredBaseRevision = 0,
    this.scope = 0,
    this.scopeIndex = 0,
    this.scopeAux = 0,
    this.layout = false,
    required this.playbackOnEntry,
    required this.topology,
    required this.frameCount,
    required this.selectedFrame,
    required this.bodyBytes,
    required this.bodyCrc32,
    required this.cursor,
    required this.sparkleSeed,
    required this.favoriteColors,
    required this.bytes,
  });

  final int session;
  final int generation;
  final int requestId;
  final int status;
  final bool active;
  final bool playing;
  final bool predecessorDeltas;
  final bool singleFrame;
  final int liveRevision, documentEpoch, coveredBaseRevision;
  final int scope, scopeIndex, scopeAux;
  final bool layout;
  bool get live => liveRevision != 0;
  final bool playbackOnEntry;
  final int topology;
  final int frameCount;
  final int selectedFrame;
  final int bodyBytes;
  final int bodyCrc32;
  final int cursor;
  final int sparkleSeed;
  final List<int> favoriteColors;
  final Uint8List bytes;

  factory LitchcraftFrameStoreSnapshotChunk.fromPacket(
    LitchcraftV2Packet packet,
  ) {
    if (packet.event != LitchcraftFrameStoreProtocol.storeSnapshotChunk) {
      throw ArgumentError.value(packet, 'packet', 'Expected Store snapshot');
    }
    final payload = Uint8List.fromList(packet.payload);
    if (payload.length < 47) {
      throw const FormatException('Truncated Store snapshot metadata');
    }
    final live = payload[1] & 64 != 0;
    if (live && payload.length < 65) {
      throw const FormatException('Truncated live Store metadata');
    }
    final status = payload[0];
    if (status != 0) {
      throw LitchcraftRejected(status);
    }
    final data = ByteData.sublistView(payload);
    final colors = <int>[
      for (var offset = 23; offset < 47; offset += 3)
        payload[offset] << 16 | payload[offset + 1] << 8 | payload[offset + 2],
    ];
    return LitchcraftFrameStoreSnapshotChunk(
      session: packet.session,
      generation: packet.revision,
      requestId: packet.requestId,
      status: status,
      active: payload[1] & 1 != 0,
      playing: payload[1] & 2 != 0,
      predecessorDeltas: payload[1] & 8 != 0,
      singleFrame: payload[1] & 32 != 0,
      layout: payload[1] & 128 != 0,
      liveRevision: live ? data.getUint32(47, Endian.little) : 0,
      documentEpoch: live ? data.getUint32(51, Endian.little) : 0,
      coveredBaseRevision: live ? data.getUint32(55, Endian.little) : 0,
      scope: live ? payload[59] : 0,
      scopeIndex: live ? data.getUint16(60, Endian.little) : 0,
      scopeAux: live ? data.getUint16(62, Endian.little) : 0,
      playbackOnEntry: payload[1] & 16 != 0,
      topology: payload[2],
      frameCount: data.getUint16(3, Endian.little),
      selectedFrame: data.getUint16(5, Endian.little),
      bodyBytes: data.getUint32(7, Endian.little),
      bodyCrc32: data.getUint32(11, Endian.little),
      cursor: data.getUint32(15, Endian.little),
      sparkleSeed: data.getUint32(19, Endian.little),
      favoriteColors: List<int>.unmodifiable(colors),
      bytes: Uint8List.fromList(payload.sublist(live ? 65 : 47)),
    );
  }
}

/// One frozen selected-frame observation; it cannot be adopted as a document.
class LitchcraftFrameStoreFramePreview {
  const LitchcraftFrameStoreFramePreview({
    required this.frame,
    required this.topology,
    required this.frameCount,
    required this.selectedFrame,
    required this.session,
    required this.generation,
  });

  final LitchcraftAnimationFrame frame;
  final LitchcraftTopology topology;
  final int frameCount, selectedFrame, session, generation;
}

class LitchcraftFrameStoreSnapshot {
  const LitchcraftFrameStoreSnapshot({
    required this.document,
    required this.session,
    required this.generation,
    required this.selectedFrame,
    required this.active,
    required this.playing,
    this.liveRevision = 0,
    this.documentEpoch = 0,
  });

  final LitchcraftAnimationDocument document;
  final int session;
  final int generation;
  final int selectedFrame;
  final bool active;
  final bool playing;
  final int liveRevision, documentEpoch;
}

/// Content identity of one canonical absolute record, including its header.
/// Immutable frame instances share this derived evidence without retaining them.
class LitchcraftFrameStoreFrameSignature {
  const LitchcraftFrameStoreFrameSignature(this.bytes, this.crc32);
  final int bytes, crc32;
  static final _cache = Expando<LitchcraftFrameStoreFrameSignature>();

  static LitchcraftFrameStoreFrameSignature forFrame(
    LitchcraftAnimationFrame frame,
  ) => _cache[frame] ??= _measure(frame);

  static LitchcraftFrameStoreFrameSignature _measure(
    LitchcraftAnimationFrame frame,
  ) {
    final encoded = encodeLitchcraftWireFrame(previous: null, frame: frame);
    final record = Uint8List(6 + encoded.length);
    final data = ByteData.sublistView(record);
    data.setUint16(0, encoded.length, Endian.little);
    data.setUint32(2, litchcraftAnimationCrc32(encoded), Endian.little);
    record.setRange(6, record.length, encoded);
    return LitchcraftFrameStoreFrameSignature(
      record.length,
      litchcraftAnimationCrc32(record),
    );
  }

  @override
  bool operator ==(Object other) =>
      other is LitchcraftFrameStoreFrameSignature &&
      bytes == other.bytes &&
      crc32 == other.crc32;
  @override
  int get hashCode => Object.hash(bytes, crc32);
}

class LitchcraftFrameStoreLiveLayout {
  const LitchcraftFrameStoreLiveLayout(this.metadata, this.frames);
  final LitchcraftFrameStoreSnapshotChunk metadata;
  final List<LitchcraftFrameStoreFrameSignature> frames;
}

class LitchcraftFrameStoreTransferResult {
  const LitchcraftFrameStoreTransferResult({
    required this.state,
    required this.bodyBytes,
    required this.chunkPackets,
    required this.checkpoints,
    required this.writes,
    required this.beginWriteMilliseconds,
    required this.bulkWriteMilliseconds,
    required this.checkpointMilliseconds,
    required this.commitWriteMilliseconds,
    required this.receiptWaitMilliseconds,
    required this.elapsedMilliseconds,
  });

  final LitchcraftFrameStoreState state;
  final int bodyBytes;
  final int chunkPackets;
  final int checkpoints;
  final int writes;
  final int beginWriteMilliseconds;
  final int bulkWriteMilliseconds;
  final int checkpointMilliseconds;
  final int commitWriteMilliseconds;
  final int receiptWaitMilliseconds;
  final int elapsedMilliseconds;
}

Future<T> _waitForReceiptOrTransportFailure<T>(
  Future<void> transport,
  Future<T> receipt,
) {
  final completion = Completer<T>();
  unawaited(
    transport.then<void>(
      (_) {},
      onError: (Object error, StackTrace stackTrace) {
        if (!completion.isCompleted) {
          completion.completeError(error, stackTrace);
        }
      },
    ),
  );
  unawaited(
    receipt.then<void>(
      (value) {
        if (!completion.isCompleted) completion.complete(value);
      },
      onError: (Object error, StackTrace stackTrace) {
        if (!completion.isCompleted) {
          completion.completeError(error, stackTrace);
        }
      },
    ),
  );
  return completion.future;
}

/// Windowed schema-4 sender over one connection-owned notification stream.
/// A successful Begin and bounded no-response chunks are pipelined before a
/// checkpoint at smaller negotiated payloads. The high-MTU shared session uses
/// cumulative Object Progress as its admission barrier and reserves its one
/// durable receipt for Commit. A short transfer queues Begin and body within
/// one semantic window, then waits for their cumulative acceptance and rechecks
/// supersession before submitting Commit separately. A high-MTU V3 shared
/// session uses 54 ATT244 chunks; smaller-MTU
/// paths retain the conservative 32-chunk window and checkpoint
/// receipts.
/// Begin errors still produce Store state, so the first explicit barrier or
/// Commit observes them without adding a healthy-path round trip.
class LitchcraftFrameStoreClient {
  LitchcraftFrameStoreClient({
    required this.packets,
    required this.writeControl,
    required this.writeBulk,
    this.writeBulkBatch,
    this.writeTransferPackets,
    this.objectSubmissionSummary,
    required this.requestReceipt,
    this.cancelWriteIsAuthoritative = false,
    required this.monotonicMilliseconds,
    this.responseTimeout = const Duration(seconds: 6),
    this.appTrace,
  });

  static const int maximumWindowChunks = 32;
  static const int highMtuWindowChunks = 54;
  static const int highMtuMinimumPacketBytes = 226;
  // Limit each native call to 48 packets. One high-MTU window may queue that
  // call plus a tail of at most six, occupying all 54 granted positions.
  // The Session owns receiver credit, native readiness and fairness across
  // these calls; both complete before the producer advances to another window.
  static const int maximumNativeBatchPackets = 48;
  // The shared session retains eight reverse responses until cumulative
  // Progress confirms them. Match that negotiated credit instead of flooding
  // the domain ingress queue with reverse objects while the forward Object
  // window is being used for artwork.
  /// Returns the largest semantic admission window safe for this transport.
  ///
  /// Only a batched V3 Object path may use the 54-chunk high-MTU window.
  /// Smaller payloads and unbatched test adapters keep the 32-chunk bound so
  /// the receiver's queue pressure stays within its proven limit.
  static int semanticWindowChunksFor({
    required int maximumPacketBytes,
    required bool sharedSession,
  }) {
    if (sharedSession &&
        maximumPacketBytes >= highMtuMinimumPacketBytes &&
        maximumPacketBytes <= 512) {
      return highMtuWindowChunks;
    }
    return maximumWindowChunks;
  }

  final Stream<LitchcraftV2Packet> packets;
  final Future<void> Function(List<int>) writeControl;
  final Future<void> Function(List<int>) writeBulk;
  final Future<void> Function(List<List<int>>)? writeBulkBatch;
  // The shared Session may retire unissued transfer messages while joining
  // their already-issued native and admission obligations before Cancel.
  final Future<void> Function(
    List<List<int>> packets, {
    required bool bulk,
    Future<void>? cancellationFuture,
    void Function(int admittedPackets)? onProgress,
  })?
  writeTransferPackets;
  // Optional transport observations only; no transfer or recovery policy.
  final ({int request, int submissions, int objects}) Function()?
  objectSubmissionSummary;
  final Future<void> Function(int request) requestReceipt;
  final bool cancelWriteIsAuthoritative;
  final int Function() monotonicMilliseconds;
  final Duration responseTimeout;
  final AppTraceSink? appTrace;

  Future<LitchcraftFrameStoreState> inspect({
    required int session,
    required int request,
  }) async {
    final inbox = _FrameStoreStateInbox(
      packets,
      // A zero-session Inspect is the cold capability probe. Firmware answers
      // with its authoritative projection session, which the caller must use
      // for any subsequent mutating transfer.
      session: session == 0 ? null : session,
      request: request,
    );
    try {
      await writeControl(
        LitchcraftFrameStoreProtocol.capabilityProbe(
          session: session,
          request: request,
        ),
      );
      final state = _requireOk((await inbox.waitFor(1, responseTimeout)).last);
      if (state.residentListing == null) {
        throw const FormatException(
          'Capability 7 Store Inspect omitted resident listing',
        );
      }
      return state;
    } finally {
      await inbox.close();
    }
  }

  Future<LitchcraftFrameStoreResidents> residents({
    required int session,
    required int request,
  }) => _requestResidents(
    LitchcraftFrameStoreProtocol.residentsRequest(
      session: session,
      request: request,
    ),
    request,
  );

  Future<LitchcraftFrameStoreResidents> selectResident({
    required int session,
    required int request,
    required LitchcraftFrameStoreResident resident,
  }) => _requestResidents(
    LitchcraftFrameStoreProtocol.selectResidentRequest(
      session: session,
      request: request,
      resident: resident,
    ),
    request,
  );

  Future<LitchcraftFrameStoreResidents> _requestResidents(
    Uint8List packet,
    int request,
  ) async {
    final completion = Completer<LitchcraftFrameStoreResidents>();
    late final StreamSubscription<LitchcraftV2Packet> subscription;
    subscription = packets.listen((response) {
      if (response.requestId != request || completion.isCompleted) return;
      if (response.event == LitchcraftFrameStoreProtocol.storeResidentsState) {
        try {
          final residents = LitchcraftFrameStoreResidents.fromPacket(response);
          if (residents.status == 0) {
            completion.complete(residents);
          } else {
            completion.completeError(LitchcraftRejected(residents.status));
          }
        } catch (error, stackTrace) {
          completion.completeError(error, stackTrace);
        }
      }
    }, onError: completion.completeError);
    try {
      // Attach the error listener before writeBulk: scripted transports and a
      // fast board can deliver a negative Residents receipt synchronously.
      // Awaiting the write first briefly leaves completeError unobserved and
      // lets an otherwise recoverable stale-resident rejection escape its
      // caller's try/catch through the Zone.
      final receipt = completion.future.timeout(responseTimeout);
      unawaited(
        receipt.then<void>((_) {}, onError: (Object _, StackTrace _) {}),
      );
      await writeBulk(packet);
      return await receipt;
    } finally {
      await subscription.cancel();
    }
  }

  /// [cancellationFuture] retires the local reader only. Snapshot requests are
  /// not upload transfers and must never send the upload Cancel record.
  Future<LitchcraftFrameStoreSnapshot> snapshot({
    required int session,
    required int generation,
    required int Function() allocateRequest,
    void Function(int received, int total)? onProgress,
    void Function(int cursor, int attempt)? onRetry,
    Future<void>? cancellationFuture,
  }) async {
    final startedAt = monotonicMilliseconds();
    final read = await _readSnapshot(
      session: session,
      generation: generation,
      allocateRequest: allocateRequest,
      onProgress: onProgress,
      onRetry: onRetry,
      cancellationFuture: cancellationFuture,
    );
    final metadata = read.metadata;
    final snapshot = LitchcraftFrameStoreSnapshot(
      document: LitchcraftAnimationDocument(
        frames: read.frames,
        topology: LitchcraftTopology.fromId(metadata.topology),
        favoriteColors: metadata.favoriteColors,
        playbackOnEntry: metadata.playbackOnEntry,
        sparkleSeed: metadata.sparkleSeed,
      ),
      session: metadata.session,
      generation: metadata.generation,
      selectedFrame: metadata.selectedFrame,
      active: metadata.active,
      playing: metadata.playing,
    );
    appTrace?.record(
      'litchcraft.frame_store_snapshot_read',
      fields: {
        'read_kind': 'saved_body',
        'session': metadata.session,
        'generation': metadata.generation,
        'request_id': read.request,
        'frames': metadata.frameCount,
        'body_bytes': read.bodyBytes,
        'body_crc32': metadata.bodyCrc32,
        'chunks': read.chunks,
        'cache_hits': 0,
        'elapsed_ms': monotonicMilliseconds() - startedAt,
      },
    );
    return snapshot;
  }

  /// Reads only the frame selected when the Store worker admits this request.
  /// Playback may advance while that frozen record drains; no runtime or draft
  /// state is changed. The same Object reader owns bounds, CRC and retirement.
  Future<LitchcraftFrameStoreFramePreview> framePreview({
    required int session,
    required int generation,
    required int Function() allocateRequest,
    Future<void>? cancellationFuture,
  }) async {
    final read = await _readSnapshot(
      session: session,
      generation: generation,
      allocateRequest: allocateRequest,
      singleFrame: true,
      cancellationFuture: cancellationFuture,
    );
    return LitchcraftFrameStoreFramePreview(
      frame: read.frames.single,
      topology: LitchcraftTopology.fromId(read.metadata.topology),
      frameCount: read.metadata.frameCount,
      selectedFrame: read.metadata.selectedFrame,
      session: read.metadata.session,
      generation: read.metadata.generation,
    );
  }

  /// Verifies one common Object stream; saved, frame and layout readers all
  /// retain the same cursor, bounds, cancellation and resume owner.
  Future<
    ({
      LitchcraftFrameStoreSnapshotChunk metadata,
      Uint8List bytes,
      int chunks,
      int request,
    })
  >
  _readSnapshotBytes({
    required int session,
    required int generation,
    required int Function() allocateRequest,
    bool singleFrame = false,
    bool layout = false,
    int? frame,
    int? liveRevision,
    void Function(int received, int total)? onProgress,
    void Function(int cursor, int attempt)? onRetry,
    Future<void>? cancellationFuture,
  }) async {
    var cancelled = false;
    final cancellation = cancellationFuture?.then((_) => cancelled = true);
    void checkCancellation() {
      if (cancelled) throw const LitchcraftTransferCancelled();
    }

    Future<T> wait<T>(Future<T> work) async {
      checkCancellation();
      if (cancellation == null) return work;
      return Future.any<T>([
        work,
        cancellation.then<T>((_) => throw const LitchcraftTransferCancelled()),
      ]);
    }

    final body = BytesBuilder(copy: false);
    var request = allocateRequest();
    final initialRequest = request;
    LitchcraftFrameStoreSnapshotChunk? first;
    var cursor = 0;
    var chunks = 0;
    var resumeAttempts = 0;
    final iterator = StreamIterator<LitchcraftV2Packet>(
      packets.where((packet) => packet.requestId == request),
    );
    var next = iterator.moveNext();
    Future<void> requestFrom(int requestedCursor) async {
      try {
        checkCancellation();
        await wait(
          writeControl(
            LitchcraftFrameStoreProtocol.snapshotRequest(
              session: session,
              request: request,
              generation: generation,
              cursor: requestedCursor,
              frame: layout
                  ? 0xfffe
                  : singleFrame
                  ? (first?.selectedFrame ?? frame ?? 0xffff)
                  : null,
              liveRevision: liveRevision,
            ),
          ).timeout(responseTimeout),
        );
      } on TimeoutException {
        throw TimeoutException(
          'Frame-store snapshot request admission stalled: '
          'cursor=$requestedCursor request=$request',
          responseTimeout,
        );
      }
    }

    try {
      await requestFrom(0);
      while (true) {
        bool received;
        try {
          received = await wait(next.timeout(responseTimeout));
          checkCancellation();
        } on TimeoutException {
          if (resumeAttempts >= 2) {
            throw TimeoutException(
              'Frame-store snapshot response stalled: '
              'cursor=$cursor bytes=${first?.bodyBytes ?? 'unknown'} '
              'request=$request attempts=$resumeAttempts',
              responseTimeout,
            );
          }
          resumeAttempts++;
          onRetry?.call(cursor, resumeAttempts);
          request = allocateRequest();
          await requestFrom(cursor);
          continue;
        }
        if (!received) break;
        final packet = iterator.current;
        if (packet.requestId != request) {
          next = iterator.moveNext();
          continue;
        }
        if (packet.event == LitchcraftFrameStoreProtocol.storeState &&
            packet.payload.isNotEmpty &&
            packet.payload.first != 0) {
          throw LitchcraftRejected(packet.payload.first);
        }
        if (packet.event != LitchcraftFrameStoreProtocol.storeSnapshotChunk) {
          next = iterator.moveNext();
          continue;
        }
        final response = LitchcraftFrameStoreSnapshotChunk.fromPacket(packet);
        if (first == null &&
            (response.session != session ||
                response.generation != generation ||
                response.cursor != 0 ||
                response.singleFrame != singleFrame ||
                response.layout != layout ||
                response.live != (liveRevision != null) ||
                (liveRevision != null &&
                    (response.liveRevision != liveRevision ||
                        response.documentEpoch == 0)) ||
                response.bodyBytes >
                    256 * (litchcraftWireMaximumFrameBytes + 6) ||
                (layout && response.bodyBytes != response.frameCount * 6) ||
                (singleFrame &&
                    frame != null &&
                    response.selectedFrame != frame) ||
                (singleFrame &&
                    (response.predecessorDeltas ||
                        response.frameCount < 1 ||
                        response.frameCount > 256 ||
                        response.selectedFrame >= response.frameCount ||
                        response.bodyBytes < 9 ||
                        response.bodyBytes >
                            litchcraftWireMaximumFrameBytes + 6)))) {
          throw const FormatException('Snapshot read changed authority');
        }
        first ??= response;
        final metadata = first;
        if (response.session != metadata.session ||
            response.generation != metadata.generation ||
            response.cursor != cursor ||
            response.bodyBytes != metadata.bodyBytes ||
            response.bodyCrc32 != metadata.bodyCrc32 ||
            response.frameCount != metadata.frameCount ||
            response.topology != metadata.topology ||
            response.sparkleSeed != metadata.sparkleSeed ||
            response.predecessorDeltas != metadata.predecessorDeltas ||
            response.singleFrame != metadata.singleFrame ||
            response.layout != metadata.layout ||
            response.liveRevision != metadata.liveRevision ||
            response.documentEpoch != metadata.documentEpoch ||
            response.coveredBaseRevision != metadata.coveredBaseRevision ||
            response.scope != metadata.scope ||
            response.scopeIndex != metadata.scopeIndex ||
            response.scopeAux != metadata.scopeAux ||
            (singleFrame && response.selectedFrame != metadata.selectedFrame) ||
            response.playbackOnEntry != metadata.playbackOnEntry ||
            !listEquals(response.favoriteColors, metadata.favoriteColors) ||
            response.bytes.isEmpty ||
            cursor + response.bytes.length > response.bodyBytes) {
          throw const FormatException('Incoherent frame-store snapshot chunk');
        }
        body.add(response.bytes);
        cursor += response.bytes.length;
        chunks++;
        resumeAttempts = 0;
        onProgress?.call(cursor, metadata.bodyBytes);
        checkCancellation();
        if (cursor == metadata.bodyBytes) break;
        next = iterator.moveNext();
      }
    } finally {
      await iterator.cancel();
    }
    checkCancellation();
    final metadata = first;
    if (metadata == null || cursor != metadata.bodyBytes) {
      throw const FormatException('Incomplete frame-store snapshot stream');
    }
    final bodyBytes = body.takeBytes();
    if (bodyBytes.length != metadata.bodyBytes ||
        litchcraftAnimationCrc32(bodyBytes) != metadata.bodyCrc32) {
      throw const FormatException('Frame-store snapshot CRC mismatch');
    }
    return (
      metadata: metadata,
      bytes: bodyBytes,
      chunks: chunks,
      request: initialRequest,
    );
  }

  List<LitchcraftAnimationFrame> _decodeSnapshotFrames(
    LitchcraftFrameStoreSnapshotChunk metadata,
    Uint8List bodyBytes, {
    required bool singleFrame,
  }) {
    final frames = <LitchcraftAnimationFrame>[];
    var offset = 0;
    while (frames.length < (singleFrame ? 1 : metadata.frameCount)) {
      if (offset + 6 > bodyBytes.length) {
        throw const FormatException('Truncated frame-store record');
      }
      final data = ByteData.sublistView(bodyBytes);
      final length = data.getUint16(offset, Endian.little);
      final crc = data.getUint32(offset + 2, Endian.little);
      offset += 6;
      if (length == 0 || offset + length > bodyBytes.length) {
        throw const FormatException('Invalid frame-store record length');
      }
      final encoded = Uint8List.sublistView(bodyBytes, offset, offset + length);
      if (litchcraftAnimationCrc32(encoded) != crc) {
        throw const FormatException('Frame-store record CRC mismatch');
      }
      final previous = frames.isEmpty
          ? null
          : metadata.predecessorDeltas
          ? frames.last
          : frames.first;
      frames.add(decodeLitchcraftWireFrame(previous: previous, bytes: encoded));
      offset += length;
    }
    if (offset != bodyBytes.length) {
      throw const FormatException('Trailing frame-store snapshot bytes');
    }
    return frames;
  }

  Future<
    ({
      LitchcraftFrameStoreSnapshotChunk metadata,
      List<LitchcraftAnimationFrame> frames,
      int bodyBytes,
      int chunks,
      int request,
    })
  >
  _readSnapshot({
    required int session,
    required int generation,
    required int Function() allocateRequest,
    bool singleFrame = false,
    void Function(int received, int total)? onProgress,
    void Function(int cursor, int attempt)? onRetry,
    Future<void>? cancellationFuture,
  }) async {
    final read = await _readSnapshotBytes(
      session: session,
      generation: generation,
      allocateRequest: allocateRequest,
      singleFrame: singleFrame,
      onProgress: onProgress,
      onRetry: onRetry,
      cancellationFuture: cancellationFuture,
    );
    return (
      metadata: read.metadata,
      bodyBytes: read.bytes.length,
      chunks: read.chunks,
      request: read.request,
      frames: _decodeSnapshotFrames(
        read.metadata,
        read.bytes,
        singleFrame: singleFrame,
      ),
    );
  }

  Future<LitchcraftFrameStoreLiveLayout> liveLayout({
    required int session,
    required int generation,
    required int liveRevision,
    required int Function() allocateRequest,
    Future<void>? cancellationFuture,
  }) async {
    final read = await _readSnapshotBytes(
      session: session,
      generation: generation,
      liveRevision: liveRevision,
      allocateRequest: allocateRequest,
      layout: true,
      cancellationFuture: cancellationFuture,
    );
    final data = ByteData.sublistView(read.bytes);
    final rows = <LitchcraftFrameStoreFrameSignature>[];
    for (var offset = 0; offset < read.bytes.length; offset += 6) {
      final length = data.getUint16(offset, Endian.little);
      if (length < 9 || length > litchcraftWireMaximumFrameBytes + 6) {
        throw const FormatException('Invalid live frame signature');
      }
      rows.add(
        LitchcraftFrameStoreFrameSignature(
          length,
          data.getUint32(offset + 2, Endian.little),
        ),
      );
    }
    return LitchcraftFrameStoreLiveLayout(
      read.metadata,
      List<LitchcraftFrameStoreFrameSignature>.unmodifiable(rows),
    );
  }

  /// Reconstructs the exact live layout, reusing immutable known frames by
  /// canonical content identity and reading only records absent from that set.
  /// The caller must still fence boot, source and current authority on adoption.
  Future<LitchcraftFrameStoreSnapshot> liveSnapshot({
    required int session,
    required int generation,
    required int liveRevision,
    required int Function() allocateRequest,
    required Iterable<LitchcraftAnimationFrame> knownFrames,
    Future<void>? cancellationFuture,
    void Function(int verifiedBytes, int totalBytes, int frameCount)?
    onProgress,
  }) async {
    final startedAt = monotonicMilliseconds();
    var cancelled = false;
    cancellationFuture?.then<void>((_) => cancelled = true);
    void checkCancellation() {
      if (cancelled) throw const LitchcraftTransferCancelled();
    }

    final layout = await liveLayout(
      session: session,
      generation: generation,
      liveRevision: liveRevision,
      allocateRequest: allocateRequest,
      cancellationFuture: cancellationFuture,
    );
    final metadata = layout.metadata;
    final known =
        <LitchcraftFrameStoreFrameSignature, LitchcraftAnimationFrame>{
          for (final frame in knownFrames)
            LitchcraftFrameStoreFrameSignature.forFrame(frame): frame,
        };
    checkCancellation();
    final totalBytes =
        metadata.bodyBytes +
        layout.frames.fold<int>(0, (total, frame) => total + frame.bytes);
    var verifiedBytes = metadata.bodyBytes;
    void progress() {
      checkCancellation();
      onProgress?.call(verifiedBytes, totalBytes, metadata.frameCount);
    }

    progress();
    final frames = <LitchcraftAnimationFrame>[];
    var bodyBytes = 0;
    var chunks = 0;
    var cacheHits = 0;
    for (var index = 0; index < layout.frames.length; index++) {
      checkCancellation();
      final signature = layout.frames[index];
      var frame = known[signature];
      if (frame == null) {
        final read = await _readSnapshotBytes(
          session: session,
          generation: generation,
          liveRevision: liveRevision,
          allocateRequest: allocateRequest,
          singleFrame: true,
          frame: index,
          cancellationFuture: cancellationFuture,
        );
        final other = read.metadata;
        if (other.documentEpoch != metadata.documentEpoch ||
            other.frameCount != metadata.frameCount ||
            other.topology != metadata.topology ||
            other.sparkleSeed != metadata.sparkleSeed ||
            other.playbackOnEntry != metadata.playbackOnEntry ||
            other.coveredBaseRevision != metadata.coveredBaseRevision ||
            other.scope != metadata.scope ||
            other.scopeIndex != metadata.scopeIndex ||
            other.scopeAux != metadata.scopeAux ||
            !listEquals(other.favoriteColors, metadata.favoriteColors) ||
            read.bytes.length != signature.bytes ||
            litchcraftAnimationCrc32(read.bytes) != signature.crc32) {
          throw const FormatException(
            'Live Store frame changed layout authority',
          );
        }
        frame = _decodeSnapshotFrames(
          other,
          read.bytes,
          singleFrame: true,
        ).single;
        bodyBytes += read.bytes.length;
        chunks += read.chunks;
        known[signature] = frame;
      } else {
        cacheHits++;
      }
      frames.add(frame);
      verifiedBytes += signature.bytes;
      progress();
    }
    checkCancellation();
    final snapshot = LitchcraftFrameStoreSnapshot(
      document: LitchcraftAnimationDocument(
        frames: frames,
        topology: LitchcraftTopology.fromId(metadata.topology),
        favoriteColors: metadata.favoriteColors,
        playbackOnEntry: metadata.playbackOnEntry,
        sparkleSeed: metadata.sparkleSeed,
      ),
      session: session,
      generation: generation,
      selectedFrame: metadata.selectedFrame,
      active: metadata.active,
      playing: metadata.playing,
      liveRevision: metadata.liveRevision,
      documentEpoch: metadata.documentEpoch,
    );
    appTrace?.record(
      'litchcraft.frame_store_snapshot_read',
      fields: {
        'read_kind': 'live_frames',
        'session': session,
        'generation': generation,
        'live_revision': liveRevision,
        'frames': frames.length,
        'body_bytes': bodyBytes,
        'metadata_bytes': metadata.bodyBytes,
        'chunks': chunks,
        'cache_hits': cacheHits,
        'elapsed_ms': monotonicMilliseconds() - startedAt,
      },
    );
    return snapshot;
  }

  Future<LitchcraftFrameStoreTransferResult> send(
    LitchcraftFrameStoreTransfer transfer, {
    required int maximumPacketBytes,
    void Function(int admitted, int total)? onProgress,
    bool Function()? isCancelled,
    Future<void>? cancellationFuture,
  }) async {
    final inbox = _FrameStoreStateInbox(
      packets,
      session: transfer.session,
      request: transfer.transfer,
    );
    final startedAt = monotonicMilliseconds();
    final skipIntermediateCheckpoints =
        writeBulkBatch != null &&
        maximumPacketBytes >= highMtuMinimumPacketBytes &&
        maximumPacketBytes <= 512;
    var writes = 0;
    var chunks = 0;
    var checkpoints = 0;
    var submitted = 0;
    var admittedBodyBytes = 0;
    var reportedBodyBytes = 0;
    var progressActive = true;
    var beginWriteMilliseconds = 0;
    var bulkWriteMilliseconds = 0;
    var checkpointMilliseconds = 0;
    var commitWriteMilliseconds = 0;
    var receiptWaitMilliseconds = 0;
    var commitSubmitted = false;
    var commitAcceptedPending = false;
    var pendingReceiptOwnsInbox = false;
    var cancelled = false;
    final cancellation = cancellationFuture?.then((_) => cancelled = true);
    Future<T> waitBeforeCommit<T>(Future<T> work) {
      if (commitSubmitted || cancellation == null) return work;
      return Future.any<T>([
        work,
        cancellation.then<T>((_) => throw const LitchcraftTransferCancelled()),
      ]);
    }

    void throwIfCancelled() {
      if (cancelled || (isCancelled?.call() ?? false)) {
        throw const LitchcraftTransferCancelled();
      }
    }

    Future<void> transferControl(List<int> packet) =>
        writeTransferPackets?.call(
          [packet],
          bulk: false,
          cancellationFuture: cancellationFuture,
        ) ??
        writeControl(packet);

    void reportProgress(int completed) {
      if (!progressActive ||
          cancelled ||
          (isCancelled?.call() ?? false) ||
          completed <= reportedBodyBytes) {
        return;
      }
      reportedBodyBytes = completed;
      onProgress?.call(completed, transfer.bodyBytes);
    }

    Future<void> transferBatch(List<List<int>> packets) {
      var admittedPackets = 0;
      return writeTransferPackets?.call(
            packets,
            bulk: true,
            cancellationFuture: cancellationFuture,
            onProgress: (admitted) {
              while (admittedPackets < admitted) {
                final packet = packets[admittedPackets++];
                if (packet[2] == LitchcraftFrameStoreProtocol.storeChunk) {
                  admittedBodyBytes += packet.length - 13;
                }
              }
              reportProgress(admittedBodyBytes);
            },
          ) ??
          writeBulkBatch!(packets);
    }

    Future<void> submitPackets(List<List<int>> packets) async {
      final batch = writeBulkBatch;
      if (batch == null) {
        for (final packet in packets) {
          if (writeTransferPackets != null) {
            await writeTransferPackets!(
              [packet],
              bulk: true,
              cancellationFuture: cancellationFuture,
            );
          } else {
            await writeBulk(packet);
          }
          throwIfCancelled();
        }
        return;
      }
      if (skipIntermediateCheckpoints &&
          packets.length <= highMtuWindowChunks) {
        // Queue only this existing 54-packet window's 48+6. Session owns grants,
        // native serialization and fairness. Future.wait observes each lazy
        // Future.sync immediately and joins all errors before Cancel can run.
        await Future.wait<void>(
          Iterable<Future<void>>.generate(
            (packets.length + maximumNativeBatchPackets - 1) ~/
                maximumNativeBatchPackets,
            (index) => Future<void>.sync(() {
              final start = index * maximumNativeBatchPackets;
              final end = (start + maximumNativeBatchPackets).clamp(
                0,
                packets.length,
              );
              return transferBatch(packets.sublist(start, end));
            }),
          ),
        );
        throwIfCancelled();
        return;
      }
      for (
        var start = 0;
        start < packets.length;
        start += maximumNativeBatchPackets
      ) {
        final end = (start + maximumNativeBatchPackets).clamp(
          0,
          packets.length,
        );
        await transferBatch(packets.sublist(start, end));
        throwIfCancelled();
      }
    }

    Future<List<LitchcraftFrameStoreState>> receipt(
      List<int> barrier, {
      Future<void>? nativeWrite,
    }) async {
      Future<List<LitchcraftFrameStoreState>> wait() async {
        if (barrier[2] == LitchcraftFrameStoreProtocol.storeCheckpoint) {
          final cursor = ByteData.sublistView(
            Uint8List.fromList(barrier),
          ).getUint32(9, Endian.little);
          await inbox.waitForCheckpoint(cursor, responseTimeout);
          return const [];
        }
        return inbox.waitFor(
          1,
          responseTimeout,
          generationAfter: transfer.expectedGeneration,
        );
      }

      try {
        final waiting = wait();
        return await (nativeWrite == null
            ? waitBeforeCommit(waiting)
            : _waitForReceiptOrTransportFailure(nativeWrite, waiting));
      } on TimeoutException {
        // The shared session retains the exact latest domain receipt. Query it
        // without replaying a durable barrier or promoting another generation.
        await waitBeforeCommit(requestReceipt(transfer.transfer));
        try {
          return await waitBeforeCommit(wait());
        } on TimeoutException {
          if (barrier[2] != LitchcraftFrameStoreProtocol.storeCommit ||
              !commitSubmitted) {
            rethrow;
          }
          // Cancel is still useful before admission. A Busy answer for this
          // exact transfer instead proves that Commit was accepted and the
          // board owns durable publication. Keep the original receipt inbox.
          final previousStates = inbox.length;
          await writeControl(transfer.cancelPacket());
          final cancelState = await inbox.waitForNextState(
            previousStates,
            responseTimeout,
          );
          if (cancelState.status == 0 &&
              _isNewerFrameStoreGeneration(
                cancelState.generation,
                transfer.expectedGeneration,
              )) {
            return <LitchcraftFrameStoreState>[cancelState];
          }
          if (cancelState.status == 0) rethrow;
          if (cancelState.status != 1) {
            throw LitchcraftRejected(cancelState.status);
          }
          commitAcceptedPending = true;
          await requestReceipt(transfer.transfer);
          try {
            return <LitchcraftFrameStoreState>[
              await inbox.waitForCommitted(
                transfer.expectedGeneration,
                responseTimeout,
              ),
            ];
          } on TimeoutException {
            pendingReceiptOwnsInbox = true;
            final completion = inbox
                .waitForCommitted(transfer.expectedGeneration)
                .then((state) {
                  _requireOk(state);
                  _requireCommittedTransfer(transfer, state);
                  return LitchcraftFrameStoreTransferResult(
                    state: state,
                    bodyBytes: transfer.bodyBytes,
                    chunkPackets: chunks,
                    checkpoints: checkpoints,
                    writes: writes,
                    beginWriteMilliseconds: beginWriteMilliseconds,
                    bulkWriteMilliseconds: bulkWriteMilliseconds,
                    checkpointMilliseconds: checkpointMilliseconds,
                    commitWriteMilliseconds: commitWriteMilliseconds,
                    receiptWaitMilliseconds:
                        monotonicMilliseconds() - startedAt,
                    elapsedMilliseconds: monotonicMilliseconds() - startedAt,
                  );
                })
                .whenComplete(inbox.close);
            // The controller attaches its observer after this method unwinds.
            unawaited(completion.then<void>((_) {}, onError: (Object _) {}));
            throw LitchcraftFrameStoreCommitPending(completion, inbox.close);
          }
        }
      }
    }

    try {
      throwIfCancelled();
      final payloadCapacity = maximumPacketBytes - 13;
      if (maximumPacketBytes < 20 || maximumPacketBytes > 512) {
        throw ArgumentError.value(maximumPacketBytes, 'maximumPacketBytes');
      }
      final chunkPacketCount =
          (transfer.bodyBytes + payloadCapacity - 1) ~/ payloadCapacity;
      final semanticWindowChunks = semanticWindowChunksFor(
        maximumPacketBytes: maximumPacketBytes,
        sharedSession: writeBulkBatch != null,
      );
      if (writeBulkBatch != null &&
          chunkPacketCount + 2 <= semanticWindowChunks) {
        final transaction = <List<int>>[
          transfer.beginPacket(),
          ...transfer.chunkPackets(maximumPacketBytes: maximumPacketBytes),
        ];
        final phaseStartedAt = monotonicMilliseconds();
        // A newer artwork may supersede this short transfer while its first
        // native batch drains. Keep Commit behind this admission boundary so
        // the inactive candidate cannot briefly become the displayed root.
        await submitPackets(transaction);
        bulkWriteMilliseconds += monotonicMilliseconds() - phaseStartedAt;
        writes = transaction.length;
        chunks = chunkPacketCount;
        submitted = transfer.bodyBytes;
        reportProgress(submitted);
        final commitStartedAt = monotonicMilliseconds();
        final nativeCommit = transferBatch(<List<int>>[
          transfer.commitPacket(),
        ]);
        commitSubmitted = true;
        commitWriteMilliseconds += monotonicMilliseconds() - commitStartedAt;
        writes++;
        final receiptStartedAt = monotonicMilliseconds();
        final responses = await receipt(
          transfer.commitPacket(),
          nativeWrite: nativeCommit,
        );
        throwIfCancelled();
        receiptWaitMilliseconds += monotonicMilliseconds() - receiptStartedAt;
        for (final state in responses) {
          _requireOk(state);
        }
        final committed = responses.last;
        _requireCommittedTransfer(transfer, committed);
        return LitchcraftFrameStoreTransferResult(
          state: committed,
          bodyBytes: transfer.bodyBytes,
          chunkPackets: chunks,
          checkpoints: checkpoints,
          writes: writes,
          beginWriteMilliseconds: beginWriteMilliseconds,
          bulkWriteMilliseconds: bulkWriteMilliseconds,
          checkpointMilliseconds: checkpointMilliseconds,
          commitWriteMilliseconds: commitWriteMilliseconds,
          receiptWaitMilliseconds: receiptWaitMilliseconds,
          elapsedMilliseconds: monotonicMilliseconds() - startedAt,
        );
      }

      var phaseStartedAt = monotonicMilliseconds();
      await transferControl(transfer.beginPacket());
      throwIfCancelled();
      beginWriteMilliseconds += monotonicMilliseconds() - phaseStartedAt;
      writes++;
      final iterator = transfer
          .chunkPackets(maximumPacketBytes: maximumPacketBytes)
          .iterator;
      var hasChunk = iterator.moveNext();
      while (hasChunk) {
        final window = <List<int>>[];
        while (hasChunk && window.length < semanticWindowChunks) {
          window.add(iterator.current);
          hasChunk = iterator.moveNext();
        }
        phaseStartedAt = monotonicMilliseconds();
        await submitPackets(window);
        // A navigation, replacement, disconnect, or newer document may retire
        // this transfer while a bounded native batch is draining. Stop at the
        // first admission boundary and let the catch path send one priority
        // Cancel before any checkpoint or Commit can extend stale work.
        throwIfCancelled();
        bulkWriteMilliseconds += monotonicMilliseconds() - phaseStartedAt;
        writes += window.length;
        chunks += window.length;
        for (final packet in window) {
          submitted += packet.length - 13;
          reportProgress(submitted);
        }
        if (hasChunk && !skipIntermediateCheckpoints) {
          phaseStartedAt = monotonicMilliseconds();
          await transferControl(
            transfer.checkpointPacket(completedBodyBytes: submitted),
          );
          checkpointMilliseconds += monotonicMilliseconds() - phaseStartedAt;
          writes++;
          checkpoints++;
          phaseStartedAt = monotonicMilliseconds();
          final responses = await receipt(
            transfer.checkpointPacket(completedBodyBytes: submitted),
          );
          receiptWaitMilliseconds += monotonicMilliseconds() - phaseStartedAt;
          for (final state in responses) {
            _requireOk(state);
          }
          throwIfCancelled();
        }
      }
      throwIfCancelled();
      phaseStartedAt = monotonicMilliseconds();
      await transferControl(transfer.commitPacket());
      commitSubmitted = true;
      commitWriteMilliseconds += monotonicMilliseconds() - phaseStartedAt;
      writes++;
      phaseStartedAt = monotonicMilliseconds();
      final responses = await receipt(transfer.commitPacket());
      receiptWaitMilliseconds += monotonicMilliseconds() - phaseStartedAt;
      for (final state in responses) {
        _requireOk(state);
      }
      final committed = responses.last;
      _requireCommittedTransfer(transfer, committed);
      return LitchcraftFrameStoreTransferResult(
        state: committed,
        bodyBytes: transfer.bodyBytes,
        chunkPackets: chunks,
        checkpoints: checkpoints,
        writes: writes,
        beginWriteMilliseconds: beginWriteMilliseconds,
        bulkWriteMilliseconds: bulkWriteMilliseconds,
        checkpointMilliseconds: checkpointMilliseconds,
        commitWriteMilliseconds: commitWriteMilliseconds,
        receiptWaitMilliseconds: receiptWaitMilliseconds,
        elapsedMilliseconds: monotonicMilliseconds() - startedAt,
      );
    } catch (_) {
      progressActive = false;
      if (commitAcceptedPending) rethrow;
      final previousResponses = inbox.length;
      try {
        await writeControl(transfer.cancelPacket());
        if (!cancelWriteIsAuthoritative) {
          final responses = await inbox.waitFor(
            previousResponses + 1,
            responseTimeout,
          );
          _requireOk(responses.last);
        }
      } catch (_) {}
      rethrow;
    } finally {
      progressActive = false;
      if (!pendingReceiptOwnsInbox) await inbox.close();
    }
  }

  static LitchcraftFrameStoreState _requireOk(LitchcraftFrameStoreState state) {
    if (state.status != 0) throw LitchcraftRejected(state.status);
    return state;
  }

  static void _requireCommittedTransfer(
    LitchcraftFrameStoreTransfer transfer,
    LitchcraftFrameStoreState committed,
  ) {
    if (!_isNewerFrameStoreGeneration(
          committed.generation,
          transfer.expectedGeneration,
        ) ||
        !committed.hasArtwork ||
        committed.frameCount != transfer.frameCount ||
        committed.selectedFrame >= transfer.frameCount ||
        committed.topology != transfer.topology) {
      throw FormatException(
        'Incoherent committed frame-store state: '
        'generation=${committed.generation}/>'
        '${transfer.expectedGeneration} '
        'artwork=${committed.hasArtwork}/true '
        'frames=${committed.frameCount}/${transfer.frameCount} '
        'selected=${committed.selectedFrame}/<${transfer.frameCount} '
        'topology=${committed.topology}/${transfer.topology}',
      );
    }
  }
}

class _FrameStoreStateInbox {
  _FrameStoreStateInbox(
    Stream<LitchcraftV2Packet> packets, {
    required this.session,
    required this.request,
  }) {
    _subscription = packets.listen(
      (packet) {
        if ((session != null && packet.session != session) ||
            packet.requestId != request) {
          return;
        }
        if (packet.event == LitchcraftFrameStoreProtocol.storeState) {
          _states.add(LitchcraftFrameStoreState.fromPacket(packet));
          if (packet.payload.first != 0) {
            _failure = LitchcraftRejected(packet.payload.first);
          }
        } else if (packet.event ==
            LitchcraftV2Protocol.frameStoreCheckpointReceipt) {
          if (packet.payload.first != 0) {
            _failure = LitchcraftRejected(packet.payload.first);
          } else {
            final cursor = ByteData.sublistView(
              Uint8List.fromList(packet.payload),
            ).getUint32(1, Endian.little);
            if (cursor > (_checkpointCursor ?? -1)) _checkpointCursor = cursor;
          }
        } else if (packet.event == LitchcraftV2Protocol.state &&
            packet.payload.isNotEmpty &&
            packet.payload.first != 0) {
          // Older firmware refuses the additive Inspect with its existing State
          // event. Preserve that actionable answer instead of timing out.
          _failure = LitchcraftRejected(packet.payload.first);
        } else {
          return;
        }
        final changed = _changed;
        _changed = null;
        if (changed != null && !changed.isCompleted) changed.complete();
      },
      onError: (Object error) {
        _failure = error;
        final changed = _changed;
        _changed = null;
        if (changed != null && !changed.isCompleted) changed.complete();
      },
      onDone: () {
        _closed = true;
        final changed = _changed;
        _changed = null;
        if (changed != null && !changed.isCompleted) changed.complete();
      },
    );
  }

  final int? session;
  final int request;
  final List<LitchcraftFrameStoreState> _states = [];
  late final StreamSubscription<LitchcraftV2Packet> _subscription;
  Completer<void>? _changed;
  Object? _failure;
  bool _closed = false;
  int? _checkpointCursor;

  int get length => _states.length;

  Future<LitchcraftFrameStoreState> waitForNextState(
    int previousCount,
    Duration timeout,
  ) async {
    while (_states.length <= previousCount) {
      if (_closed) throw const LitchcraftTransferCancelled();
      if (_failure case final failure?) throw failure;
      final changed = Completer<void>();
      _changed = changed;
      if (_states.length > previousCount) break;
      await changed.future.timeout(timeout);
    }
    return _states[previousCount];
  }

  Future<LitchcraftFrameStoreState> waitForCommitted(
    int previousGeneration, [
    Duration? timeout,
  ]) async {
    while (true) {
      for (final state in _states) {
        if (state.status == 0 &&
            _isNewerFrameStoreGeneration(
              state.generation,
              previousGeneration,
            )) {
          return state;
        }
      }
      if (_closed) throw const LitchcraftTransferCancelled();
      final changed = Completer<void>();
      _changed = changed;
      if (timeout == null) {
        await changed.future;
      } else {
        await changed.future.timeout(timeout);
      }
    }
  }

  Future<List<LitchcraftFrameStoreState>> waitFor(
    int count,
    Duration timeout, {
    int? generationAfter,
  }) async {
    List<LitchcraftFrameStoreState> matching() => _states
        .where(
          (state) =>
              generationAfter == null ||
              _isNewerFrameStoreGeneration(state.generation, generationAfter) ||
              state.status != 0,
        )
        .toList();
    while (matching().length < count) {
      if (_closed) throw const LitchcraftTransferCancelled();
      if (_failure case final failure?) throw failure;
      final changed = Completer<void>();
      _changed = changed;
      if (matching().length >= count) {
        _changed = null;
        break;
      }
      try {
        await changed.future.timeout(timeout);
      } on TimeoutException {
        final observed = _states
            .map(
              (state) =>
                  'status=${state.status} generation=${state.generation} '
                  'frames=${state.frameCount} active=${state.active} '
                  'playing=${state.playing}',
            )
            .join('; ');
        throw TimeoutException(
          'Frame-store receipt timeout: expected=$count '
          'received=${_states.length} states=[$observed]',
          timeout,
        );
      }
    }
    return List<LitchcraftFrameStoreState>.unmodifiable(matching().take(count));
  }

  Future<void> waitForCheckpoint(int cursor, Duration timeout) async {
    // Keep one cursor watermark, not an unbounded receipt ledger. Duplicate
    // earlier receipts cannot satisfy the next FIFO window's barrier.
    while (_checkpointCursor != cursor) {
      if (_closed) throw const LitchcraftTransferCancelled();
      if (_failure case final failure?) throw failure;
      final changed = Completer<void>();
      _changed = changed;
      await changed.future.timeout(timeout);
    }
    if (_failure case final failure?) throw failure;
  }

  Future<void> close() async {
    if (_closed) return;
    _closed = true;
    final changed = _changed;
    _changed = null;
    if (changed != null && !changed.isCompleted) {
      changed.complete();
    }
    await _subscription.cancel();
  }
}
