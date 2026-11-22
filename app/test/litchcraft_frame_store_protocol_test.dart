import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/foundation.dart' show listEquals;
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/models/litchcraft_animation_codec.dart';
import 'package:litch_mobile/src/models/litchcraft_bouncing_balls_preset.dart';
import 'package:litch_mobile/src/models/litchcraft_wire_codec.dart';
import 'package:litch_mobile/src/services/app_trace_recorder.dart';
import 'package:litch_mobile/src/services/connection_litchcraft_codec.dart';
import 'package:litch_mobile/src/services/litchcraft_frame_store_protocol.dart';
import 'package:litch_mobile/src/services/litchcraft_protocol.dart';

class _SnapshotTrace implements AppTraceSink {
  final reads = <Map<String, Object?>>[];

  @override
  void record(String event, {Map<String, Object?> fields = const {}}) {
    if (event == 'litchcraft.frame_store_snapshot_read') {
      reads.add(Map.of(fields));
    }
  }
}

LitchcraftAnimationFrame _frame(int seed, {int changes = 256}) {
  return LitchcraftAnimationFrame(
    durationMs: 50 + seed % 4951,
    emitters: List<LitchcraftAnimatedEmitter>.generate(256, (index) {
      if (index >= changes) return const LitchcraftAnimatedEmitter(color: 0);
      return LitchcraftAnimatedEmitter(
        color:
            (((seed + index * 11) & 0xff) << 16) |
            (((seed + index * 17) & 0xff) << 8) |
            ((seed + index * 23) & 0xff),
      );
    }),
  );
}

LitchcraftFrameStoreTransfer _transfer(
  List<LitchcraftAnimationFrame> frames, {
  bool enableRgbWire = false,
}) => LitchcraftFrameStoreProtocol.prepare(
  session: 0x12345678,
  transfer: 0x4321,
  expectedGeneration: 9,
  enableRgbWire: enableRgbWire,
  topology: 3,
  active: true,
  playbackOnEntry: true,
  sparkleSeed: 7,
  favoriteColors: const <int>[
    0x010203,
    0x040506,
    0x070809,
    0x0a0b0c,
    0x0d0e0f,
    0x101112,
    0x131415,
    0x161718,
  ],
  frames: frames,
);

Uint8List _statePacket({
  int event = LitchcraftFrameStoreProtocol.storeState,
  int session = 0x12345678,
  int generation = 9,
  int request = 0x4321,
  int status = 0,
  int flags = 23,
  int frameCount = 256,
  int selectedFrame = 255,
}) {
  final packet = Uint8List(19);
  final data = ByteData.sublistView(packet);
  packet[0] = LitchcraftV2Protocol.magic;
  packet[1] = LitchcraftV2Protocol.version;
  packet[2] = event;
  data.setUint32(3, session, Endian.little);
  data.setUint32(7, generation, Endian.little);
  data.setUint16(11, request, Endian.little);
  packet[13] = status;
  packet[14] = flags;
  data.setUint16(15, frameCount, Endian.little);
  data.setUint16(17, selectedFrame, Endian.little);
  return packet;
}

Uint8List _inspectStatePacketWithResidents({
  int session = 0x12345678,
  int request = 0x4321,
}) {
  final packet = Uint8List(111);
  packet.setRange(0, 19, _statePacket(session: session, request: request));
  packet[19] = 1;
  packet[20] = 2;
  final data = ByteData.sublistView(packet);
  for (var slot = 0; slot < 2; slot++) {
    final offset = 21 + slot * 45;
    packet[offset] = slot;
    data.setUint32(offset + 1, slot == 0 ? 9 : 14, Endian.little);
    data.setUint32(
      offset + 5,
      slot == 0 ? 0x11223344 : 0x55667788,
      Endian.little,
    );
    data.setUint32(offset + 9, slot == 0 ? 22000 : 28000, Endian.little);
    data.setUint16(offset + 13, 256, Endian.little);
    packet[offset + 15] = 3;
    packet[offset + 16] = 7;
    data.setUint32(offset + 17, 7 + slot, Endian.little);
    for (var color = 0; color < 8; color++) {
      packet[offset + 21 + color * 3] = color + slot;
      packet[offset + 22 + color * 3] = color + slot + 1;
      packet[offset + 23 + color * 3] = color + slot + 2;
    }
  }
  return packet;
}

LitchcraftV2Packet _decodedState({
  int generation = 9,
  int request = 0x4321,
  int flags = 23,
  int frameCount = 256,
  int selectedFrame = 0,
}) => LitchcraftV2Packet.decode(
  _statePacket(
    generation: generation,
    request: request,
    flags: flags,
    frameCount: frameCount,
    selectedFrame: selectedFrame,
  ),
)!;

LitchcraftV2Packet _checkpointReceipt(List<int> request) => LitchcraftV2Packet(
  LitchcraftV2Protocol.frameStoreCheckpointReceipt,
  0x12345678,
  9,
  0x4321,
  [0, ...request.sublist(9, 13)],
);

Uint8List _snapshotBody(
  List<LitchcraftAnimationFrame> frames, {
  bool independent = false,
}) {
  final body = BytesBuilder(copy: false);
  LitchcraftAnimationFrame? previous;
  for (final frame in frames) {
    final encoded = encodeLitchcraftWireFrame(
      previous: independent ? null : previous,
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

LitchcraftV2Packet _snapshotPacket({
  required Uint8List body,
  required int request,
  required int cursor,
  int? bodyCrc32,
  int frameCount = 2,
  int chunkBytes = 170,
  bool throughSharedObject = false,
  bool singleFrame = false,
  int selectedFrame = 1,
}) {
  final count = (body.length - cursor).clamp(0, chunkBytes);
  final packet = Uint8List(60 + count);
  final data = ByteData.sublistView(packet);
  packet[0] = LitchcraftV2Protocol.magic;
  packet[1] = LitchcraftV2Protocol.version;
  packet[2] = LitchcraftFrameStoreProtocol.storeSnapshotChunk;
  data.setUint32(3, 0x12345678, Endian.little);
  data.setUint32(7, 9, Endian.little);
  data.setUint16(11, request, Endian.little);
  packet[13] = 0;
  packet[14] = singleFrame ? (31 & ~8) | 32 : 31;
  packet[15] = 3;
  data.setUint16(16, frameCount, Endian.little);
  data.setUint16(18, selectedFrame, Endian.little);
  data.setUint32(20, body.length, Endian.little);
  data.setUint32(
    24,
    bodyCrc32 ?? litchcraftAnimationCrc32(body),
    Endian.little,
  );
  data.setUint32(28, cursor, Endian.little);
  data.setUint32(32, 7, Endian.little);
  for (var index = 0; index < 8; index++) {
    packet[36 + index * 3] = index + 1;
    packet[37 + index * 3] = index + 2;
    packet[38 + index * 3] = index + 3;
  }
  packet.setRange(60, packet.length, body, cursor);
  if (throughSharedObject) {
    final assembler = ConnectionLitchcraftAssembler();
    Uint8List? completed;
    for (final fragment in ConnectionLitchcraftCodec.encodePacket(
      packet,
      maximumBodyBytes: 502,
    )) {
      completed = assembler.accept(fragment);
    }
    return LitchcraftV2Packet.decode(completed!)!;
  }
  if (selectedFrame >= frameCount) {
    // Exercise the typed reader defensively; the ingress decoder rejects this
    // malformed field separately in the same test.
    return LitchcraftV2Packet(
      packet[2],
      0x12345678,
      9,
      request,
      packet.sublist(13),
    );
  }
  return LitchcraftV2Packet.decode(packet)!;
}

LitchcraftV2Packet _residentsPacket({
  required int request,
  int selectedRoot = 1,
  int status = 0,
}) {
  final packet = Uint8List(106);
  final data = ByteData.sublistView(packet);
  packet[0] = LitchcraftV2Protocol.magic;
  packet[1] = LitchcraftV2Protocol.version;
  packet[2] = LitchcraftFrameStoreProtocol.storeResidentsState;
  data.setUint32(3, 0x12345678, Endian.little);
  data.setUint32(7, selectedRoot == 0 ? 9 : 14, Endian.little);
  data.setUint16(11, request, Endian.little);
  packet[13] = status;
  packet[14] = selectedRoot;
  packet[15] = 2;
  for (var slot = 0; slot < 2; slot++) {
    final offset = 16 + slot * 45;
    packet[offset] = slot;
    data.setUint32(offset + 1, slot == 0 ? 9 : 14, Endian.little);
    data.setUint32(
      offset + 5,
      slot == 0 ? 0x11223344 : 0x55667788,
      Endian.little,
    );
    data.setUint32(offset + 9, slot == 0 ? 22000 : 28000, Endian.little);
    data.setUint16(offset + 13, 256, Endian.little);
    packet[offset + 15] = 3;
    packet[offset + 16] = 7;
    data.setUint32(offset + 17, 7 + slot, Endian.little);
    for (var color = 0; color < 8; color++) {
      packet[offset + 21 + color * 3] = color + slot;
      packet[offset + 22 + color * 3] = color + slot + 1;
      packet[offset + 23 + color * 3] = color + slot + 2;
    }
  }
  return LitchcraftV2Packet.decode(packet)!;
}

void main() {
  for (final frameCount in [1, 32, 256]) {
    for (final rgbWire in [false, true]) {
      test(
        'pure content identity matches canonical transfer frames=$frameCount rgb=$rgbWire',
        () {
          final frames = [
            for (var index = 0; index < frameCount; index++) _frame(index),
          ];
          final transfer = _transfer(frames, enableRgbWire: rgbWire);
          final document = LitchcraftAnimationDocument(
            frames: frames,
            topology: LitchcraftTopology.v3,
            favoriteColors: transfer.favoriteColors,
            playbackOnEntry: transfer.playbackOnEntry,
            sparkleSeed: transfer.sparkleSeed,
          );
          final identity = LitchcraftFrameStoreProtocol.identify(document);
          expect(identity.bodyBytes, transfer.canonicalBodyBytes);
          expect(identity.bodyCrc32, transfer.canonicalBodyCrc32);
          expect(identity.frameCount, frameCount);
          expect(identity.topology, transfer.topology);
          expect(identity.playbackOnEntry, transfer.playbackOnEntry);
          expect(identity.sparkleSeed, transfer.sparkleSeed);
        },
      );
    }
  }

  test(
    'closed Store receipt stream retires checkpoint wait promptly',
    () async {
      final notifications = StreamController<LitchcraftV2Packet>.broadcast(
        sync: true,
      );
      final checkpoint = Completer<void>();
      final transfer = _transfer([
        for (var i = 0; i < 256; i++) _frame(i, changes: 10),
      ]);
      final client = LitchcraftFrameStoreClient(
        packets: notifications.stream,
        responseTimeout: const Duration(seconds: 6),
        writeControl: (bytes) async {
          if (bytes[2] == LitchcraftFrameStoreProtocol.storeCheckpoint &&
              !checkpoint.isCompleted) {
            checkpoint.complete();
          }
        },
        writeBulk: (_) async {},
        requestReceipt: (_) async {},
        monotonicMilliseconds: () => 0,
      );
      final work = client.send(transfer, maximumPacketBytes: 244);
      final failure = expectLater(
        work,
        throwsA(isA<LitchcraftTransferCancelled>()),
      );
      await checkpoint.future;
      await notifications.close();
      await failure.timeout(const Duration(milliseconds: 300));
    },
  );

  test('precommit supersession cancels a missing checkpoint receipt', () async {
    final notifications = StreamController<LitchcraftV2Packet>.broadcast(
      sync: true,
    );
    final retired = Completer<void>();
    final checkpoint = Completer<void>();
    final operations = <int>[];
    final transfer = _transfer([
      for (var i = 0; i < 256; i++) _frame(i, changes: 10),
    ]);
    final client = LitchcraftFrameStoreClient(
      packets: notifications.stream,
      cancelWriteIsAuthoritative: true,
      writeControl: (bytes) async {
        operations.add(bytes[2]);
        if (bytes[2] == LitchcraftFrameStoreProtocol.storeCheckpoint &&
            !checkpoint.isCompleted) {
          checkpoint.complete();
        }
      },
      writeBulk: (_) async {},
      requestReceipt: (_) async {},
      monotonicMilliseconds: () => 0,
    );
    final work = client.send(
      transfer,
      maximumPacketBytes: 244,
      cancellationFuture: retired.future,
    );
    final failure = expectLater(
      work,
      throwsA(isA<LitchcraftTransferCancelled>()),
    );
    await checkpoint.future;
    retired.complete();
    await failure.timeout(const Duration(milliseconds: 300));
    expect(operations.last, LitchcraftFrameStoreProtocol.storeCancel);
    expect(
      operations,
      isNot(contains(LitchcraftFrameStoreProtocol.storeCommit)),
    );
    await notifications.close();
  });

  for (final holdQuery in [false, true]) {
    test(
      'precommit supersession cancels replay wait with held Query=$holdQuery',
      () async {
        final notifications = StreamController<LitchcraftV2Packet>.broadcast(
          sync: true,
        );
        final retired = Completer<void>();
        final queryEntered = Completer<void>();
        final releaseQuery = Completer<void>();
        addTearDown(() {
          if (!releaseQuery.isCompleted) releaseQuery.complete();
        });
        final operations = <int>[];
        final transfer = _transfer([
          for (var i = 0; i < 256; i++) _frame(i, changes: 10),
        ]);
        final client = LitchcraftFrameStoreClient(
          packets: notifications.stream,
          responseTimeout: const Duration(milliseconds: 50),
          cancelWriteIsAuthoritative: true,
          writeControl: (bytes) async {
            operations.add(bytes[2]);
          },
          writeBulk: (_) async {},
          requestReceipt: (_) async {
            queryEntered.complete();
            if (holdQuery) await releaseQuery.future;
          },
          monotonicMilliseconds: () => 0,
        );
        final work = client.send(
          transfer,
          maximumPacketBytes: 244,
          cancellationFuture: retired.future,
        );
        final failure = expectLater(
          work,
          throwsA(isA<LitchcraftTransferCancelled>()),
        );
        await queryEntered.future;
        await Future<void>.delayed(Duration.zero);
        retired.complete();
        try {
          await failure.timeout(const Duration(milliseconds: 30));
        } finally {
          if (!releaseQuery.isCompleted) releaseQuery.complete();
        }
        expect(operations.last, LitchcraftFrameStoreProtocol.storeCancel);
        expect(
          operations.where(
            (op) => op == LitchcraftFrameStoreProtocol.storeCancel,
          ),
          hasLength(1),
        );
        expect(
          operations,
          isNot(contains(LitchcraftFrameStoreProtocol.storeCommit)),
        );
        await notifications.close();
      },
    );
  }

  test(
    'frame preview reads one independently verified frame from a 256-frame root',
    () async {
      final frame = _frame(108);
      final body = _snapshotBody([frame]);
      final packets = StreamController<LitchcraftV2Packet>.broadcast(
        sync: true,
      );
      final writes = <List<int>>[];
      final client = LitchcraftFrameStoreClient(
        packets: packets.stream,
        writeControl: (bytes) async {
          writes.add(bytes);
          expect(bytes, hasLength(19));
          expect(
            ByteData.sublistView(
              Uint8List.fromList(bytes),
            ).getUint16(17, Endian.little),
            0xffff,
          );
          final request = bytes[7] | bytes[8] << 8;
          for (var cursor = 0; cursor < body.length; cursor += 170) {
            packets.add(
              _snapshotPacket(
                body: body,
                request: request,
                cursor: cursor,
                frameCount: 256,
                selectedFrame: 108,
                singleFrame: true,
                throughSharedObject: true,
              ),
            );
          }
        },
        writeBulk: (_) async => fail('preview must not send document bytes'),
        requestReceipt: (_) async => fail('preview is not an upload'),
        monotonicMilliseconds: () => 0,
      );
      final preview = await client.framePreview(
        session: 0x12345678,
        generation: 9,
        allocateRequest: () => 1,
      );
      expect(preview.frameCount, 256);
      expect(preview.selectedFrame, 108);
      expect(preview.frame.durationMs, frame.durationMs);
      expect(
        preview.frame.emitters.map(
          (e) => (e.color, e.effect, e.effectDurationMs),
        ),
        orderedEquals(
          frame.emitters.map((e) => (e.color, e.effect, e.effectDurationMs)),
        ),
      );
      expect(
        body.length,
        lessThanOrEqualTo(litchcraftWireMaximumFrameBytes + 6),
      );
      expect(writes, hasLength(1));
      await packets.close();
    },
  );

  for (final fault in [
    'full document',
    'wrong selection',
    'oversize',
    'CRC',
    'changed frame',
  ]) {
    test('frame preview rejects $fault without adopting artwork', () async {
      final body = _snapshotBody([_frame(108)]);
      final packets = StreamController<LitchcraftV2Packet>.broadcast(
        sync: true,
      );
      final client = LitchcraftFrameStoreClient(
        packets: packets.stream,
        writeControl: (bytes) async {
          final request = bytes[7] | bytes[8] << 8;
          for (var cursor = 0; cursor < body.length; cursor += 170) {
            final encoded = _snapshotPacket(
              body: body,
              request: request,
              cursor: cursor,
              frameCount: 256,
              selectedFrame: fault == 'wrong selection'
                  ? 256
                  : fault == 'changed frame' && cursor > 0
                  ? 109
                  : 108,
              singleFrame: fault != 'full document',
            );
            final raw = Uint8List(13 + encoded.payload.length);
            raw[0] = LitchcraftV2Protocol.magic;
            raw[1] = LitchcraftV2Protocol.version;
            raw[2] = encoded.event;
            final header = ByteData.sublistView(raw);
            header.setUint32(3, encoded.session, Endian.little);
            header.setUint32(7, encoded.revision, Endian.little);
            header.setUint16(11, encoded.requestId, Endian.little);
            raw.setRange(13, raw.length, encoded.payload);
            if (fault == 'oversize') {
              ByteData.sublistView(raw).setUint32(20, 1130, Endian.little);
            }
            if (fault == 'CRC' && cursor == 0) raw[24] ^= 1;
            if (fault == 'wrong selection') {
              expect(LitchcraftV2Packet.decode(raw), isNull);
              packets.add(
                LitchcraftV2Packet(
                  encoded.event,
                  encoded.session,
                  encoded.revision,
                  encoded.requestId,
                  raw.sublist(13),
                ),
              );
            } else {
              packets.add(LitchcraftV2Packet.decode(raw)!);
            }
          }
        },
        writeBulk: (_) async {},
        requestReceipt: (_) async {},
        monotonicMilliseconds: () => 0,
      );
      await expectLater(
        client.framePreview(
          session: 0x12345678,
          generation: 9,
          allocateRequest: () => 1,
        ),
        throwsFormatException,
      );
      await packets.close();
    });
  }

  test('shared high-MTU Store receipt decodes all 438 body bytes', () {
    final body = Uint8List.fromList(
      List<int>.generate(438, (index) => index & 0xff),
    );
    final packet = _snapshotPacket(
      body: body,
      request: 19,
      cursor: 0,
      chunkBytes: 438,
    );
    expect(packet.payload, hasLength(485));
    final chunk = LitchcraftFrameStoreSnapshotChunk.fromPacket(packet);
    expect(chunk.bytes, body);
    final oversized = Uint8List.fromList([
      ...LitchcraftFrameStoreProtocol.snapshotRequest(
        session: 0x12345678,
        request: 19,
        generation: 9,
        cursor: 0,
      ),
      ...List<int>.filled(500, 0),
    ]);
    oversized[2] = LitchcraftFrameStoreProtocol.storeSnapshotChunk;
    expect(LitchcraftV2Packet.decode(oversized), isNull);
  });

  test(
    'enumerates and selects a validated resident with one small packet',
    () async {
      final notifications = StreamController<LitchcraftV2Packet>.broadcast(
        sync: true,
      );
      final writes = <Uint8List>[];
      final client = LitchcraftFrameStoreClient(
        packets: notifications.stream,
        writeControl: (_) async {},
        writeBulk: (bytes) async {
          final packet = Uint8List.fromList(bytes);
          writes.add(packet);
          final request = ByteData.sublistView(
            packet,
          ).getUint16(7, Endian.little);
          notifications.add(
            _residentsPacket(
              request: request,
              selectedRoot:
                  packet[2] == LitchcraftFrameStoreProtocol.storeSelect
                  ? packet[9]
                  : 1,
            ),
          );
        },
        requestReceipt: (_) async {},
        monotonicMilliseconds: () => 0,
      );

      final residents = await client.residents(
        session: 0x12345678,
        request: 70,
      );
      expect(residents.selectedRootSlot, 1);
      expect(residents.residents, hasLength(2));
      expect(residents.residents.last.sparkleSeed, 8);
      expect(residents.residents.last.favoriteColors, hasLength(8));
      final alternate = residents.residents.singleWhere(
        (resident) => resident.rootSlot == 0,
      );
      final selected = await client.selectResident(
        session: 0x12345678,
        request: 71,
        resident: alternate,
      );
      expect(selected.selectedRootSlot, 0);
      expect(writes.map((packet) => packet[2]), [25, 26]);
      expect(writes.last, hasLength(18));
      expect(writes.last[9], alternate.rootSlot);
      final selectData = ByteData.sublistView(writes.last);
      expect(selectData.getUint32(10, Endian.little), alternate.generation);
      expect(selectData.getUint32(14, Endian.little), alternate.bodyCrc32);
      await notifications.close();
    },
  );

  test(
    'synchronous resident rejection is observed by the requesting caller',
    () async {
      final notifications = StreamController<LitchcraftV2Packet>.broadcast(
        sync: true,
      );
      final resident = LitchcraftFrameStoreResidents.fromPacket(
        _residentsPacket(request: 1),
      ).residents.first;
      final client = LitchcraftFrameStoreClient(
        packets: notifications.stream,
        writeControl: (_) async {},
        writeBulk: (bytes) async {
          final request = ByteData.sublistView(
            Uint8List.fromList(bytes),
          ).getUint16(7, Endian.little);
          notifications.add(_residentsPacket(request: request, status: 9));
        },
        requestReceipt: (_) async {},
        monotonicMilliseconds: () => 0,
      );

      await expectLater(
        client.selectResident(
          session: 0x12345678,
          request: 72,
          resident: resident,
        ),
        throwsA(
          isA<LitchcraftRejected>().having(
            (error) => error.status,
            'status',
            9,
          ),
        ),
      );
      await notifications.close();
    },
  );

  testWidgets('queried checkpoint receipt cannot credit a later barrier', (
    tester,
  ) async {
    final transfer = _transfer([
      for (var i = 0; i < 150; i++) _frame(i, changes: 10),
    ]);
    final notifications = StreamController<LitchcraftV2Packet>.broadcast(
      sync: true,
    );
    final operations = <int>[];
    List<int>? first, second;
    var receiptQueries = 0;
    var completed = false;
    final client = LitchcraftFrameStoreClient(
      packets: notifications.stream,
      responseTimeout: const Duration(seconds: 1),
      writeControl: (bytes) async {
        operations.add(bytes[2]);
        if (bytes[2] != LitchcraftFrameStoreProtocol.storeCheckpoint) return;
        first ??= List.of(bytes);
        if (!listEquals(first, bytes)) {
          second = List.of(bytes);
        }
      },
      writeBulk: (bytes) async => operations.add(bytes[2]),
      requestReceipt: (_) async {
        receiptQueries++;
        notifications.add(_checkpointReceipt(first!));
        notifications.add(_checkpointReceipt(first!));
      },
      monotonicMilliseconds: () => 0,
    );
    final work = client.send(transfer, maximumPacketBytes: 244).then((result) {
      completed = true;
      return result;
    });
    await tester.pump();
    await tester.pump(const Duration(seconds: 1));
    expect(second, isNotNull);
    expect(receiptQueries, 1);
    notifications.add(_checkpointReceipt(first!));
    await tester.pump();
    expect(
      operations.where((op) => op == LitchcraftFrameStoreProtocol.storeChunk),
      hasLength(64),
    );
    expect(
      operations,
      isNot(contains(LitchcraftFrameStoreProtocol.storeCommit)),
    );
    notifications.add(_checkpointReceipt(second!));
    await tester.pump();
    expect(operations.last, LitchcraftFrameStoreProtocol.storeCommit);
    notifications.add(_checkpointReceipt(first!));
    notifications.add(_checkpointReceipt(second!));
    notifications.add(
      _decodedState(generation: 9, frameCount: 150, selectedFrame: 149),
    );
    await tester.pump();
    expect(
      completed,
      isFalse,
      reason: 'only the committed generation confirms completion',
    );
    notifications.add(
      _decodedState(generation: 10, frameCount: 150, selectedFrame: 149),
    );
    await tester.pump();
    await tester.runAsync(() => Future<void>.delayed(Duration.zero));
    expect((await work).state.generation, 10);
    expect(
      operations.where((op) => op == LitchcraftFrameStoreProtocol.storeCommit),
      hasLength(1),
    );
    await notifications.close();
  });

  for (final lostOperation in [
    LitchcraftFrameStoreProtocol.storeCheckpoint,
    LitchcraftFrameStoreProtocol.storeCommit,
  ]) {
    testWidgets(
      'lost Store barrier $lostOperation queries without resending artwork',
      (tester) async {
        final transfer = _transfer(<LitchcraftAnimationFrame>[
          for (var i = 0; i < 256; i++) _frame(i, changes: 10),
        ]);
        final notifications = StreamController<LitchcraftV2Packet>.broadcast(
          sync: true,
        );
        final operations = <int>[];
        final barriers = <List<int>>[];
        final receiptQueries = <int>[];
        var lost = false;
        final client = LitchcraftFrameStoreClient(
          packets: notifications.stream,
          responseTimeout: const Duration(seconds: 1),
          writeControl: (bytes) async {
            operations.add(bytes[2]);
            if (bytes[2] == LitchcraftFrameStoreProtocol.storeBegin) return;
            barriers.add(List<int>.of(bytes));
            if (!lost && bytes[2] == lostOperation) {
              lost = true;
              return;
            }
            notifications.add(
              bytes[2] == LitchcraftFrameStoreProtocol.storeCommit
                  ? _decodedState(generation: 10)
                  : _checkpointReceipt(bytes),
            );
          },
          writeBulk: (bytes) async => operations.add(bytes[2]),
          requestReceipt: (request) async {
            receiptQueries.add(request);
            final lostBarrier = barriers.singleWhere(
              (packet) => packet[2] == lostOperation,
            );
            notifications.add(
              lostOperation == LitchcraftFrameStoreProtocol.storeCommit
                  ? _decodedState(generation: 10)
                  : _checkpointReceipt(lostBarrier),
            );
          },
          monotonicMilliseconds: () => 0,
        );
        var done = false;
        final work = client
            .send(transfer, maximumPacketBytes: 244)
            .whenComplete(() => done = true);
        for (var attempt = 0; attempt < 5 && !done; attempt++) {
          await tester.pump(const Duration(seconds: 1));
          await tester.runAsync(() => Future<void>.delayed(Duration.zero));
        }
        expect(done, isTrue, reason: 'operations=$operations');
        final result = await work;
        expect(receiptQueries, <int>[transfer.transfer]);
        final lostBarrier = barriers.firstWhere(
          (packet) => packet[2] == lostOperation,
        );
        expect(
          barriers.where((packet) => listEquals(packet, lostBarrier)),
          hasLength(1),
        );
        expect(result.state.generation, 10);
        expect(result.writes, operations.length);
        expect(
          operations.where(
            (op) => op == LitchcraftFrameStoreProtocol.storeBegin,
          ),
          hasLength(1),
        );
        expect(
          operations.where(
            (op) => op == LitchcraftFrameStoreProtocol.storeChunk,
          ),
          hasLength(115),
        );
        expect(
          operations,
          isNot(contains(LitchcraftFrameStoreProtocol.storeCancel)),
        );
        await notifications.close();
      },
    );
  }

  testWidgets(
    'shared-session receipt query recovers Commit without replaying it',
    (tester) async {
      final transfer = _transfer(<LitchcraftAnimationFrame>[_frame(1)]);
      final notifications = StreamController<LitchcraftV2Packet>.broadcast(
        sync: true,
      );
      final operations = <int>[];
      final receiptQueries = <int>[];
      final client = LitchcraftFrameStoreClient(
        packets: notifications.stream,
        responseTimeout: const Duration(seconds: 1),
        writeControl: (bytes) async => operations.add(bytes[2]),
        writeBulk: (bytes) async => operations.add(bytes[2]),
        writeBulkBatch: (packets) async =>
            operations.addAll(packets.map((packet) => packet[2])),
        requestReceipt: (request) async {
          receiptQueries.add(request);
          notifications.add(
            _decodedState(generation: 10, frameCount: 1, selectedFrame: 0),
          );
        },
        monotonicMilliseconds: () => 0,
      );

      final work = client.send(transfer, maximumPacketBytes: 244);
      await tester.pump(const Duration(seconds: 1));
      await tester.runAsync(() => Future<void>.delayed(Duration.zero));
      final result = await work;

      expect(receiptQueries, <int>[transfer.transfer]);
      expect(
        operations.where(
          (operation) => operation == LitchcraftFrameStoreProtocol.storeCommit,
        ),
        hasLength(1),
      );
      expect(result.state.generation, 10);
      expect(result.writes, operations.length);
      await notifications.close();
    },
  );

  testWidgets(
    'persistent Commit loss queries the receipt and cancels staging',
    (tester) async {
      final frame = _frame(1);
      final transfer = _transfer(List.filled(256, frame));
      final notifications = StreamController<LitchcraftV2Packet>.broadcast(
        sync: true,
      );
      final operations = <int>[];
      var receiptQueries = 0;
      final client = LitchcraftFrameStoreClient(
        packets: notifications.stream,
        responseTimeout: const Duration(seconds: 1),
        writeControl: (bytes) async {
          operations.add(bytes[2]);
          if (bytes[2] == LitchcraftFrameStoreProtocol.storeCancel) {
            notifications.add(_decodedState());
          }
        },
        writeBulk: (_) async {},
        writeBulkBatch: (packets) async =>
            operations.addAll(packets.map((p) => p[2])),
        requestReceipt: (_) async => receiptQueries++,
        monotonicMilliseconds: () => 0,
      );
      final work = client.send(transfer, maximumPacketBytes: 244);
      final failed = expectLater(work, throwsA(isA<TimeoutException>()));
      await tester.pump();
      await tester.pump(const Duration(seconds: 1));
      await tester.pump(const Duration(seconds: 1));
      await tester.runAsync(() => Future<void>.delayed(Duration.zero));
      await failed;
      expect(
        operations.where(
          (op) => op == LitchcraftFrameStoreProtocol.storeCommit,
        ),
        hasLength(1),
      );
      expect(receiptQueries, 1);
      expect(operations.last, LitchcraftFrameStoreProtocol.storeCancel);
      await notifications.close();
    },
  );

  test(
    'new user intent cancels a large Store upload after its first54-packet window',
    () async {
      final transfer = _transfer(<LitchcraftAnimationFrame>[
        for (var index = 0; index < 256; index++) _frame(index, changes: 10),
      ]);
      final notifications = StreamController<LitchcraftV2Packet>.broadcast(
        sync: true,
      );
      final operations = <int>[];
      var cancelled = false;
      var batches = 0;
      final client = LitchcraftFrameStoreClient(
        packets: notifications.stream,
        responseTimeout: const Duration(seconds: 1),
        writeControl: (bytes) async {
          operations.add(bytes[2]);
          if (bytes[2] == LitchcraftFrameStoreProtocol.storeCancel) {
            scheduleMicrotask(() => notifications.add(_decodedState()));
          }
        },
        writeBulk: (bytes) async => operations.add(bytes[2]),
        writeBulkBatch: (packets) async {
          batches++;
          operations.addAll(packets.map((packet) => packet[2]));
          cancelled = true;
        },
        requestReceipt: (_) async {},
        monotonicMilliseconds: () => 0,
      );

      final work = client.send(
        transfer,
        maximumPacketBytes: 244,
        isCancelled: () => cancelled,
      );
      await expectLater(work, throwsA(isA<LitchcraftTransferCancelled>()));

      expect(batches, 2);
      expect(
        operations.where(
          (operation) => operation == LitchcraftFrameStoreProtocol.storeChunk,
        ),
        hasLength(LitchcraftFrameStoreClient.highMtuWindowChunks),
        reason:
            'the already queued48+6 share one54-packet window; '
            'no second window or Commit may follow new intent',
      );
      expect(
        operations,
        isNot(contains(LitchcraftFrameStoreProtocol.storeCheckpoint)),
      );
      expect(
        operations,
        isNot(contains(LitchcraftFrameStoreProtocol.storeCommit)),
      );
      expect(operations.last, LitchcraftFrameStoreProtocol.storeCancel);
      await notifications.close();
    },
  );

  for (final failure in ['synchronous', 'asynchronous']) {
    test(
      'Store window joins its queued tail after $failure writer error',
      () async {
        final transfer = _transfer([
          for (var index = 0; index < 256; index++) _frame(index, changes: 10),
        ]);
        final packets = StreamController<LitchcraftV2Packet>.broadcast(
          sync: true,
        );
        addTearDown(packets.close);
        final tail = Completer<void>();
        final head = Completer<void>();
        var calls = 0, cancels = 0, commits = 0, failed = false;
        final client = LitchcraftFrameStoreClient(
          packets: packets.stream,
          writeControl: (bytes) async {
            if (bytes[2] == LitchcraftFrameStoreProtocol.storeCancel) cancels++;
            if (bytes[2] == LitchcraftFrameStoreProtocol.storeCommit) commits++;
          },
          writeBulk: (_) async {},
          writeBulkBatch: (bytes) {
            calls++;
            if (calls == 1) {
              if (failure == 'synchronous') throw StateError('head rejected');
              return head.future;
            }
            return tail.future;
          },
          cancelWriteIsAuthoritative: true,
          requestReceipt: (_) async {},
          monotonicMilliseconds: () => 0,
        );
        final work = client.send(transfer, maximumPacketBytes: 230);
        final observed = work.then<void>(
          (_) => fail('writer failure was lost'),
          onError: (Object error) {
            expect(error, isA<StateError>());
            failed = true;
          },
        );
        await Future<void>.delayed(Duration.zero);
        expect(calls, 2);
        if (failure == 'asynchronous') {
          head.completeError(StateError('head rejected'));
        }
        await Future<void>.delayed(Duration.zero);
        expect(failed, isFalse);
        expect(cancels, 0, reason: 'tail must settle before priority Cancel');
        tail.completeError(StateError('tail rejected'));
        await observed;
        expect(failed, isTrue);
        expect(cancels, 1);
        expect(commits, 0);
        expect(calls, 2);
      },
    );
  }

  test('first schema-4 transfer uses the active mode session', () {
    expect(
      litchcraftFrameStoreAuthoritySession(documentSession: 0, modeSession: 41),
      41,
    );
    expect(
      litchcraftFrameStoreAuthoritySession(
        documentSession: 77,
        modeSession: 41,
      ),
      77,
    );
  });

  test('semantic window stays compatible below the high-MTU threshold', () {
    expect(
      LitchcraftFrameStoreClient.semanticWindowChunksFor(
        maximumPacketBytes: 244,
        sharedSession: true,
      ),
      LitchcraftFrameStoreClient.highMtuWindowChunks,
    );
    expect(
      LitchcraftFrameStoreClient.semanticWindowChunksFor(
        maximumPacketBytes: 225,
        sharedSession: true,
      ),
      LitchcraftFrameStoreClient.maximumWindowChunks,
    );
    expect(
      LitchcraftFrameStoreClient.semanticWindowChunksFor(
        maximumPacketBytes: 244,
        sharedSession: false,
      ),
      LitchcraftFrameStoreClient.maximumWindowChunks,
    );
  });

  test('exactly 256 repeated frames pack across record boundaries', () {
    final first = _frame(1);
    final frames = <LitchcraftAnimationFrame>[
      first,
      for (var index = 1; index < 256; index++)
        LitchcraftAnimationFrame(
          durationMs: 50 + index,
          emitters: first.emitters,
        ),
    ];
    final transfer = _transfer(frames);
    final begin = transfer.beginPacket();
    final chunks = transfer.chunkPackets(maximumPacketBytes: 244).toList();
    final body = <int>[];
    for (var index = 0; index < chunks.length; index++) {
      final packet = chunks[index];
      expect(packet.length, lessThanOrEqualTo(244));
      expect(packet[2], LitchcraftFrameStoreProtocol.storeChunk);
      expect(
        ByteData.sublistView(packet).getUint32(9, Endian.little),
        body.length,
      );
      body.addAll(packet.skip(13));
    }

    expect(begin, hasLength(53));
    expect(begin.sublist(0, 9), <int>[77, 2, 18, 120, 86, 52, 18, 33, 67]);
    final beginFields = ByteData.sublistView(begin);
    expect(beginFields.getUint32(9, Endian.little), 9);
    expect(beginFields.getUint32(13, Endian.little), transfer.bodyBytes);
    expect(beginFields.getUint32(17, Endian.little), transfer.bodyCrc32);
    expect(begin.sublist(25), <int>[
      7,
      0,
      0,
      0,
      for (var color = 1; color <= 24; color++) color,
    ]);
    expect(ByteData.sublistView(begin).getUint16(21, Endian.little), 256);
    expect(begin[23], 3);
    expect(
      begin[24],
      7,
      reason: 'active, playback-on-entry, and predecessor-delta flags',
    );
    expect(transfer.frameCount, 256);
    expect(transfer.bodyBytes, 11264);
    expect(chunks, hasLength(49));
    expect(
      transfer.chunkPackets(maximumPacketBytes: 512),
      hasLength(23),
      reason: 'an ATT517 link carries the same anchored body in 23 writes',
    );
    expect(body, hasLength(transfer.bodyBytes));
    expect(litchcraftAnimationCrc32(body), transfer.bodyCrc32);
    final bodyData = ByteData.sublistView(Uint8List.fromList(body));
    var recordOffset = 0;
    for (var index = 0; index < 256; index++) {
      final length = bodyData.getUint16(recordOffset, Endian.little);
      if (index % LitchcraftFrameStoreProtocol.keyframeInterval == 0) {
        expect(
          body[recordOffset + 6],
          2,
          reason: 'frame $index is independent',
        );
      }
      recordOffset += 6 + length;
    }
    expect(recordOffset, body.length);
    expect(
      transfer.checkpointPacket(completedBodyBytes: body.length),
      hasLength(13),
    );
    expect(transfer.commitPacket(), hasLength(13));
    expect(transfer.cancelPacket(), hasLength(9));
  });

  test('default and explicit legacy modes preserve exact Store bytes', () {
    final frames = <LitchcraftAnimationFrame>[_frame(1), _frame(2)];
    final implicit = _transfer(frames);
    final explicit = _transfer(frames, enableRgbWire: false);
    expect(implicit.beginPacket(), explicit.beginPacket());
    expect(implicit.beginPacket()[24], 7);
    expect(
      implicit.chunkPackets(maximumPacketBytes: 244).toList(),
      explicit.chunkPackets(maximumPacketBytes: 244).toList(),
    );
    expect(implicit.bodyBytes, implicit.canonicalBodyBytes);
    expect(implicit.bodyCrc32, implicit.canonicalBodyCrc32);
    expect(implicit.commitPacket(), explicit.commitPacket());
  });

  test('RGB wire compresses Bouncing Balls but keeps legacy root identity', () {
    final frames = LitchcraftBouncingBallsPreset.decode().frames;
    final legacy = _transfer(frames);
    final rgb = _transfer(frames, enableRgbWire: true);
    final begin = ByteData.sublistView(rgb.beginPacket());
    final wireBody = Uint8List.fromList(
      rgb
          .chunkPackets(maximumPacketBytes: 244)
          .expand((packet) => packet.skip(13))
          .toList(),
    );
    expect(legacy.bodyBytes, 196545);
    expect(rgb.bodyBytes, 122791);
    expect(rgb.canonicalBodyBytes, legacy.bodyBytes);
    expect(rgb.canonicalBodyCrc32, legacy.bodyCrc32);
    expect(rgb.beginPacket()[24], 0x0f);
    expect(begin.getUint32(13, Endian.little), rgb.bodyBytes);
    expect(begin.getUint32(17, Endian.little), rgb.bodyCrc32);
    expect(litchcraftAnimationCrc32(wireBody), rgb.bodyCrc32);
    final wireTags = <int>{};
    var cursor = 0;
    while (cursor < wireBody.length) {
      final data = ByteData.sublistView(wireBody);
      final length = data.getUint16(cursor, Endian.little);
      final encoded = Uint8List.sublistView(
        wireBody,
        cursor + 6,
        cursor + 6 + length,
      );
      expect(
        data.getUint32(cursor + 2, Endian.little),
        litchcraftAnimationCrc32(encoded),
      );
      wireTags.add(encoded[0]);
      cursor += 6 + length;
    }
    expect(cursor, rgb.bodyBytes);
    expect(wireTags, containsAll(<int>[4, 5]));
    expect(
      ByteData.sublistView(
        rgb.checkpointPacket(completedBodyBytes: cursor),
      ).getUint32(9, Endian.little),
      rgb.bodyBytes,
    );
    expect(
      () => rgb.checkpointPacket(completedBodyBytes: rgb.canonicalBodyBytes),
      throwsArgumentError,
    );
    expect(rgb.commitPacket().sublist(9), <int>[
      0xa7, 0xdf, 0x01, 0x00, // 122791 incoming bytes, little endian.
    ]);
    final resident = LitchcraftFrameStoreResident(
      rootSlot: 0,
      generation: 10,
      bodyBytes: legacy.bodyBytes,
      bodyCrc32: legacy.bodyCrc32,
      frameCount: legacy.frameCount,
      topology: legacy.topology,
      active: true,
      playbackOnEntry: legacy.playbackOnEntry,
      predecessorDeltas: true,
      sparkleSeed: legacy.sparkleSeed,
      favoriteColors: legacy.favoriteColors,
    );
    expect(resident.matches(legacy), isTrue);
    expect(resident.matches(rgb), isTrue);
  });

  test('effect-bearing frames retain legacy wire records in RGB mode', () {
    final frame = LitchcraftAnimationFrame(
      emitters: <LitchcraftAnimatedEmitter>[
        const LitchcraftAnimatedEmitter(
          color: 0x123456,
          effect: LitchcraftEffectStyle.breathe,
          effectDurationMs: 500,
        ),
        ...List<LitchcraftAnimatedEmitter>.filled(
          255,
          const LitchcraftAnimatedEmitter(color: 0),
        ),
      ],
    );
    final legacy = _transfer([frame]);
    final rgb = _transfer([frame], enableRgbWire: true);
    expect(rgb.beginPacket()[24], 0x0f);
    expect(rgb.bodyBytes, legacy.bodyBytes);
    expect(rgb.bodyCrc32, legacy.bodyCrc32);
    expect(
      rgb.chunkPackets(maximumPacketBytes: 244).toList(),
      legacy.chunkPackets(maximumPacketBytes: 244).toList(),
    );
  });

  test('capability probe and Store state are public and exact', () {
    expect(
      LitchcraftFrameStoreProtocol.capabilityProbe(
        session: 0x12345678,
        request: 0x4321,
      ),
      <int>[0x4d, 2, 1, 0x78, 0x56, 0x34, 0x12, 0x21, 0x43, 7],
    );

    final state = LitchcraftFrameStoreProtocol.decodeState(
      _statePacket(),
      expectedSession: 0x12345678,
      expectedRequest: 0x4321,
    );
    expect(state, isNotNull);
    expect(state!.generation, 9);
    expect(state.frameCount, 256);
    expect(state.selectedFrame, 255);
    expect(state.active, isTrue);
    expect(state.playing, isTrue);
    expect(state.hasArtwork, isTrue);
    expect(state.topology, 3);
    expect(state.rgbWireCapable, isFalse);
    expect(_statePacket(), hasLength(lessThanOrEqualTo(20)));

    final inspected = LitchcraftFrameStoreProtocol.decodeState(
      _inspectStatePacketWithResidents(),
    );
    expect(inspected, isNotNull);
    expect(inspected!.selectedRootSlot, 1);
    expect(inspected.residents, hasLength(2));
    expect(inspected.residents.last.bodyCrc32, 0x55667788);
    expect(inspected.residents.last.favoriteColors, hasLength(8));

    final rgbPacket = LitchcraftV2Packet.decode(_statePacket(flags: 0x37));
    expect(rgbPacket, isNotNull);
    final rgbState = LitchcraftFrameStoreState.fromPacket(rgbPacket!);
    expect(rgbState.rgbWireCapable, isTrue);
    expect(rgbState.topology, 3);
    final rgbListing = LitchcraftV2Packet.decode(
      Uint8List.fromList(_inspectStatePacketWithResidents()..[14] = 0x37),
    );
    expect(rgbListing, isNotNull);
    expect(
      LitchcraftFrameStoreState.fromPacket(rgbListing!).rgbWireCapable,
      isTrue,
    );

    final fresh = LitchcraftFrameStoreProtocol.decodeState(
      _statePacket(flags: 16, frameCount: 0, selectedFrame: 0),
    );
    expect(fresh, isNotNull);
    expect(fresh!.hasArtwork, isFalse);
    expect(fresh.frameCount, 0);
    expect(fresh.selectedFrame, 0);
    expect(fresh.topology, 3);

    final oldPeerUnsupported = LitchcraftV2Packet.decode(<int>[
      0x4d,
      2,
      LitchcraftV2Protocol.state,
      0x78,
      0x56,
      0x34,
      0x12,
      9,
      0,
      0,
      0,
      0x21,
      0x43,
      3,
      0,
      0,
      0,
      0,
    ]);
    expect(oldPeerUnsupported, isNotNull);
    expect(oldPeerUnsupported!.payload[0], 3);
  });

  test('Store state rejects malformed and stale responses', () {
    expect(
      LitchcraftFrameStoreProtocol.decodeState(
        _statePacket(),
        expectedSession: 2,
      ),
      isNull,
    );
    expect(
      LitchcraftFrameStoreProtocol.decodeState(
        _statePacket(),
        expectedRequest: 2,
      ),
      isNull,
    );
    for (final malformed in <Uint8List>[
      Uint8List.sublistView(_statePacket(), 0, 18),
      _statePacket(flags: 32),
      _statePacket(flags: 0x40 | 23),
      _statePacket(flags: 0x38 | 7),
      _statePacket(flags: 2),
      _statePacket(flags: 24),
      _statePacket(flags: 16, frameCount: 1, selectedFrame: 0),
      _statePacket(flags: 20, frameCount: 0, selectedFrame: 0),
      _statePacket(frameCount: 257),
      _statePacket(selectedFrame: 256),
      _statePacket(event: 0x8c),
    ]) {
      expect(LitchcraftFrameStoreProtocol.decodeState(malformed), isNull);
    }
    final malformedSummary = _inspectStatePacketWithResidents()..[20] = 3;
    expect(LitchcraftV2Packet.decode(malformedSummary), isNull);
    final malformedRgbSummary = _inspectStatePacketWithResidents()
      ..[14] = 0x37
      ..[19] = 2;
    expect(LitchcraftV2Packet.decode(malformedRgbSummary), isNull);
  });

  test('Store begin rejects playback without active authority', () {
    expect(
      () => LitchcraftFrameStoreProtocol.prepare(
        session: 1,
        transfer: 1,
        expectedGeneration: 0,
        topology: 3,
        active: false,
        playbackOnEntry: true,
        sparkleSeed: 1,
        favoriteColors: List<int>.filled(8, 0),
        frames: <LitchcraftAnimationFrame>[_frame(1)],
      ),
      throwsArgumentError,
    );
  });

  test('prepared transfer bytes are frozen from caller mutation', () {
    final favorites = <int>[
      0x010203,
      0x040506,
      0x070809,
      0x0a0b0c,
      0x0d0e0f,
      0x101112,
      0x131415,
      0x161718,
    ];
    final frames = <LitchcraftAnimationFrame>[_frame(1)];
    final transfer = LitchcraftFrameStoreProtocol.prepare(
      session: 1,
      transfer: 1,
      expectedGeneration: 0,
      topology: 3,
      active: true,
      playbackOnEntry: false,
      sparkleSeed: 1,
      favoriteColors: favorites,
      frames: frames,
    );
    final begin = transfer.beginPacket();
    final chunks = transfer.chunkPackets(maximumPacketBytes: 244).toList();
    favorites[0] = 0xffffff;
    frames.clear();
    expect(() => transfer.favoriteColors[0] = 0, throwsUnsupportedError);
    expect(transfer.beginPacket(), begin);
    expect(transfer.chunkPackets(maximumPacketBytes: 244), chunks);
  });

  test(
    'record framing retains every per-frame CRC without whole-body staging',
    () {
      final frames = List<LitchcraftAnimationFrame>.generate(
        256,
        (index) => _frame(index, changes: 10),
      );
      final transfer = _transfer(frames);
      final chunks = transfer.chunkPackets(maximumPacketBytes: 244).toList();
      final body = Uint8List.fromList(
        chunks.expand((packet) => packet.skip(13)).toList(),
      );
      var cursor = 0;
      var records = 0;
      while (cursor < body.length) {
        final data = ByteData.sublistView(body);
        final length = data.getUint16(cursor, Endian.little);
        final crc = data.getUint32(cursor + 2, Endian.little);
        final encoded = body.sublist(cursor + 6, cursor + 6 + length);
        expect(length, inInclusiveRange(3, 1123));
        expect(litchcraftAnimationCrc32(encoded), crc);
        cursor += 6 + length;
        records++;
      }
      expect(cursor, transfer.bodyBytes);
      expect(records, 256);
      expect(transfer.bodyBytes, 26392);
      expect(chunks, hasLength(115));
      expect(chunks.length, (transfer.bodyBytes + 230) ~/ 231);
    },
  );

  test('256-frame lossless corpora expose exact radio packet budgets', () {
    List<LitchcraftAnimationFrame> corpus(int changes) =>
        List<LitchcraftAnimationFrame>.generate(
          256,
          (index) => _frame(index, changes: changes),
        );

    for (final expectation in <(int, int, int)>[
      (1, 13000, 57),
      (256, 289024, 1252),
    ]) {
      final transfer = _transfer(corpus(expectation.$1));
      expect(transfer.bodyBytes, expectation.$2);
      expect(
        transfer.chunkPackets(maximumPacketBytes: 244),
        hasLength(expectation.$3),
      );
    }
  });

  test(
    'accumulating one-emitter animation uses predecessor deltas losslessly',
    () {
      final frames = <LitchcraftAnimationFrame>[_frame(17)];
      for (var index = 1; index < 256; index++) {
        final emitters = frames.last.emitters.toList();
        final prior = emitters[index - 1];
        emitters[index - 1] = LitchcraftAnimatedEmitter(
          color: prior.color ^ 0x5a5a5a,
          effect: prior.effect,
          effectDurationMs: prior.effectDurationMs,
        );
        frames.add(
          LitchcraftAnimationFrame(durationMs: 50 + index, emitters: emitters),
        );
      }
      final transfer = _transfer(frames);
      final chunks = transfer.chunkPackets(maximumPacketBytes: 512).toList();
      final body = Uint8List.fromList(
        chunks.expand((packet) => packet.skip(13)).toList(),
      );
      expect(transfer.beginPacket()[24], 7);
      expect(transfer.bodyBytes, 13000);
      expect(chunks, hasLength(27));

      var cursor = 0;
      LitchcraftAnimationFrame? previous;
      for (var index = 0; index < frames.length; index++) {
        final length = ByteData.sublistView(
          body,
        ).getUint16(cursor, Endian.little);
        final encoded = Uint8List.sublistView(
          body,
          cursor + 6,
          cursor + 6 + length,
        );
        expect(
          ByteData.sublistView(body).getUint32(cursor + 2, Endian.little),
          litchcraftAnimationCrc32(encoded),
        );
        final decoded = decodeLitchcraftWireFrame(
          previous: previous,
          bytes: encoded,
        );
        expect(decoded.durationMs, frames[index].durationMs);
        expect(
          decoded.emitters.map((emitter) => emitter.color),
          frames[index].emitters.map((emitter) => emitter.color),
        );
        previous = decoded;
        cursor += 6 + length;
      }
      expect(cursor, body.length);
    },
  );

  test(
    'minimum ATT remains valid and invalid envelopes fail before output',
    () {
      final transfer = _transfer(<LitchcraftAnimationFrame>[_frame(1)]);
      final chunks = transfer.chunkPackets(maximumPacketBytes: 20).toList();
      expect(chunks, everyElement(hasLength(lessThanOrEqualTo(20))));
      expect(
        chunks.expand((packet) => packet.skip(13)),
        hasLength(transfer.bodyBytes),
      );
      expect(
        () => transfer.chunkPackets(maximumPacketBytes: 19).toList(),
        throwsArgumentError,
      );
      expect(
        () => LitchcraftFrameStoreProtocol.prepare(
          session: 1,
          transfer: 1,
          expectedGeneration: 0,
          topology: 3,
          active: true,
          playbackOnEntry: false,
          sparkleSeed: 1,
          favoriteColors: const <int>[],
          frames: const <LitchcraftAnimationFrame>[],
        ),
        throwsArgumentError,
      );
    },
  );

  test('every schema-4 Begin field rejects its own invalid boundary', () {
    void invalid({
      int session = 1,
      int transfer = 1,
      int generation = 0,
      int topology = 3,
      int seed = 1,
      List<int>? favorites,
      int frameCount = 1,
    }) {
      expect(
        () => LitchcraftFrameStoreProtocol.prepare(
          session: session,
          transfer: transfer,
          expectedGeneration: generation,
          topology: topology,
          active: true,
          playbackOnEntry: false,
          sparkleSeed: seed,
          favoriteColors: favorites ?? List<int>.filled(8, 0, growable: false),
          frames: List<LitchcraftAnimationFrame>.generate(
            frameCount,
            (index) => _frame(index),
          ),
        ),
        throwsArgumentError,
      );
    }

    invalid(session: 0);
    invalid(session: 0x100000000);
    invalid(transfer: 0);
    invalid(transfer: 0x10000);
    invalid(generation: -1);
    invalid(generation: 0x100000000);
    invalid(topology: 0);
    invalid(topology: 4);
    invalid(seed: 0);
    invalid(seed: 0x100000000);
    invalid(favorites: List<int>.filled(7, 0));
    invalid(favorites: <int>[0, 0, 0, 0, 0, 0, 0, -1]);
    invalid(favorites: <int>[0, 0, 0, 0, 0, 0, 0, 0x1000000]);
    invalid(frameCount: 0);
    invalid(frameCount: 257);
  });

  test(
    'one-window 256-frame transfer fences Commit after body admission',
    () async {
      final first = _frame(1);
      final transfer = _transfer(<LitchcraftAnimationFrame>[
        first,
        for (var index = 1; index < 256; index++)
          LitchcraftAnimationFrame(
            durationMs: 50 + index,
            emitters: first.emitters,
          ),
      ]);
      final notifications = StreamController<LitchcraftV2Packet>.broadcast(
        sync: true,
      );
      final admitted = <Uint8List>[];
      var batchCalls = 0;
      var now = 100;
      final client = LitchcraftFrameStoreClient(
        packets: notifications.stream,
        writeControl: (_) => throw StateError(
          'one-window transfer must not await separate control admission',
        ),
        writeBulk: (_) => throw StateError('single-packet pacing is forbidden'),
        writeBulkBatch: (packets) async {
          batchCalls++;
          admitted.addAll(packets.map(Uint8List.fromList));
          expect(
            packets,
            hasLength(
              lessThanOrEqualTo(
                LitchcraftFrameStoreClient.maximumNativeBatchPackets,
              ),
            ),
          );
          if (admitted.last[2] == LitchcraftFrameStoreProtocol.storeCommit) {
            notifications.add(_decodedState(generation: 10));
          }
        },
        requestReceipt: (_) async {},
        monotonicMilliseconds: () => now,
      );
      final resultFuture = client.send(transfer, maximumPacketBytes: 244);
      now = 112;
      final result = await resultFuture;

      expect(result.bodyBytes, 11264);
      expect(result.chunkPackets, 49);
      expect(result.checkpoints, 0);
      expect(result.writes, 51);
      expect(result.beginWriteMilliseconds, greaterThanOrEqualTo(0));
      expect(result.bulkWriteMilliseconds, greaterThanOrEqualTo(0));
      expect(result.checkpointMilliseconds, 0);
      expect(result.commitWriteMilliseconds, greaterThanOrEqualTo(0));
      expect(result.receiptWaitMilliseconds, greaterThanOrEqualTo(0));
      expect(result.elapsedMilliseconds, 12);
      expect(batchCalls, 3);
      expect(admitted, hasLength(51));
      expect(admitted.first[2], LitchcraftFrameStoreProtocol.storeBegin);
      expect(
        admitted
            .skip(1)
            .take(49)
            .every(
              (packet) => packet[2] == LitchcraftFrameStoreProtocol.storeChunk,
            ),
        isTrue,
      );
      expect(admitted.last[2], LitchcraftFrameStoreProtocol.storeCommit);
      await notifications.close();
    },
  );

  test(
    'commit accepts the authoritative generation after a newer alternate resident',
    () async {
      final transfer = _transfer(<LitchcraftAnimationFrame>[
        _frame(1, changes: 1),
      ]);
      final notifications = StreamController<LitchcraftV2Packet>.broadcast(
        sync: true,
      );
      final client = LitchcraftFrameStoreClient(
        packets: notifications.stream,
        writeControl: (_) => throw StateError(
          'small resident replacement must share one native batch',
        ),
        writeBulk: (_) => throw StateError('single-packet pacing is forbidden'),
        writeBulkBatch: (_) async {
          // Selected A is generation 9, resident B is generation 13, so the
          // frame store correctly allocates 14 rather than assuming 10.
          notifications.add(
            _decodedState(generation: 14, frameCount: 1, selectedFrame: 0),
          );
        },
        requestReceipt: (_) async {},
        monotonicMilliseconds: () => 0,
      );

      final result = await client.send(transfer, maximumPacketBytes: 244);
      expect(result.state.generation, 14);
      await notifications.close();
    },
  );

  test(
    'authoritative Store receipt wins over a late native batch callback',
    () async {
      final first = _frame(1);
      final transfer = _transfer(<LitchcraftAnimationFrame>[
        first,
        for (var index = 1; index < 256; index++)
          LitchcraftAnimationFrame(
            durationMs: 50 + index,
            emitters: first.emitters,
          ),
      ]);
      final notifications = StreamController<LitchcraftV2Packet>.broadcast(
        sync: true,
      );
      final batchStarted = Completer<void>();
      final nativeCallback = Completer<void>();
      var now = 0;
      final client = LitchcraftFrameStoreClient(
        packets: notifications.stream,
        writeControl: (_) => throw StateError(
          'one-window transfer must not await separate control admission',
        ),
        writeBulk: (_) => throw StateError('single-packet pacing is forbidden'),
        writeBulkBatch: (packets) {
          if (packets.last[2] != LitchcraftFrameStoreProtocol.storeCommit) {
            return Future<void>.value();
          }
          batchStarted.complete();
          now = 4;
          return nativeCallback.future;
        },
        requestReceipt: (_) async {},
        monotonicMilliseconds: () => now,
      );

      var completed = false;
      final resultFuture = client
          .send(transfer, maximumPacketBytes: 244)
          .whenComplete(() => completed = true);
      await batchStarted.future;
      now = 71;
      notifications.add(_decodedState(generation: 10));
      await pumpEventQueue(times: 5);

      expect(completed, isTrue);
      expect(nativeCallback.isCompleted, isFalse);
      final result = await resultFuture;
      expect(result.state.generation, 10);
      expect(result.bulkWriteMilliseconds, 0);
      expect(result.commitWriteMilliseconds, 4);
      expect(result.receiptWaitMilliseconds, 67);
      expect(result.elapsedMilliseconds, 71);
      nativeCallback.completeError(StateError('late redundant callback error'));
      await pumpEventQueue(times: 5);
      await notifications.close();
    },
  );

  testWidgets('accepted Commit survives two receipt windows and Cancel Busy', (
    tester,
  ) async {
    final transfer = _transfer(<LitchcraftAnimationFrame>[_frame(1)]);
    final notifications = StreamController<LitchcraftV2Packet>.broadcast(
      sync: true,
    );
    var cancels = 0;
    var receiptQueries = 0;
    final client = LitchcraftFrameStoreClient(
      packets: notifications.stream,
      responseTimeout: const Duration(milliseconds: 100),
      writeControl: (bytes) async {
        if (bytes[2] != LitchcraftFrameStoreProtocol.storeCancel) return;
        cancels++;
        notifications.add(
          LitchcraftV2Packet.decode(
            _statePacket(
              status: 1,
              generation: 9,
              frameCount: 1,
              selectedFrame: 0,
            ),
          )!,
        );
      },
      writeBulk: (_) async {},
      writeBulkBatch: (_) async {},
      requestReceipt: (_) async => receiptQueries++,
      cancelWriteIsAuthoritative: true,
      monotonicMilliseconds: () => 0,
    );

    final work = client.send(transfer, maximumPacketBytes: 244);
    await tester.pump();
    for (var index = 0; index < 4 && cancels == 0; index++) {
      await tester.pump(const Duration(milliseconds: 101));
    }
    expect(cancels, 1);
    expect(receiptQueries, 2);
    notifications.add(
      _decodedState(generation: 10, frameCount: 1, selectedFrame: 0),
    );
    await tester.pump();
    final result = await tester.runAsync(() => work);
    expect(result?.state.generation, 10);
    expect(result?.state.playing, isTrue);
    await notifications.close();
  });

  testWidgets('accepted Commit retains receipt beyond the third window', (
    tester,
  ) async {
    final transfer = _transfer(<LitchcraftAnimationFrame>[_frame(1)]);
    final notifications = StreamController<LitchcraftV2Packet>.broadcast(
      sync: true,
    );
    var cancels = 0;
    final client = LitchcraftFrameStoreClient(
      packets: notifications.stream,
      responseTimeout: const Duration(milliseconds: 100),
      writeControl: (bytes) async {
        if (bytes[2] != LitchcraftFrameStoreProtocol.storeCancel) return;
        cancels++;
        notifications.add(
          LitchcraftV2Packet.decode(
            _statePacket(
              status: 1,
              generation: 9,
              frameCount: 1,
              selectedFrame: 0,
            ),
          )!,
        );
      },
      writeBulk: (_) async {},
      writeBulkBatch: (_) async {},
      requestReceipt: (_) async {},
      cancelWriteIsAuthoritative: true,
      monotonicMilliseconds: () => 0,
    );
    final work = client.send(transfer, maximumPacketBytes: 244);
    final pending = work.then<LitchcraftFrameStoreCommitPending>(
      (_) => throw TestFailure('Commit must await its durable receipt'),
      onError: (Object error) => error as LitchcraftFrameStoreCommitPending,
    );
    await tester.pump();
    for (var index = 0; index < 5; index++) {
      await tester.pump(const Duration(milliseconds: 101));
    }
    expect(cancels, 1);
    final unresolved = await tester.runAsync(() => pending);
    expect(unresolved, isNotNull);
    notifications.add(
      _decodedState(generation: 10, flags: 21, frameCount: 1, selectedFrame: 0),
    );
    final resolved = await tester.runAsync(() => unresolved!.completion);
    expect(resolved?.state.generation, 10);
    expect(resolved?.state.playing, isFalse);
    expect(resolved?.state.active, isTrue);
    await notifications.close();
  });

  testWidgets('late Commit receipt accepts current inactive Exit projection', (
    tester,
  ) async {
    final transfer = _transfer(<LitchcraftAnimationFrame>[_frame(1)]);
    final notifications = StreamController<LitchcraftV2Packet>.broadcast(
      sync: true,
    );
    final client = LitchcraftFrameStoreClient(
      packets: notifications.stream,
      responseTimeout: const Duration(milliseconds: 100),
      writeControl: (bytes) async {
        if (bytes[2] != LitchcraftFrameStoreProtocol.storeCancel) return;
        notifications.add(
          LitchcraftV2Packet.decode(
            _statePacket(
              status: 1,
              generation: 9,
              frameCount: 1,
              selectedFrame: 0,
            ),
          )!,
        );
      },
      writeBulk: (_) async {},
      writeBulkBatch: (_) async {},
      requestReceipt: (_) async {},
      cancelWriteIsAuthoritative: true,
      monotonicMilliseconds: () => 0,
    );
    final pending = client
        .send(transfer, maximumPacketBytes: 244)
        .then<LitchcraftFrameStoreCommitPending>(
          (_) => throw TestFailure('Commit must await its durable receipt'),
          onError: (Object error) => error as LitchcraftFrameStoreCommitPending,
        );
    await tester.pump();
    for (var index = 0; index < 5; index++) {
      await tester.pump(const Duration(milliseconds: 101));
    }
    final unresolved = await tester.runAsync(() => pending);
    notifications.add(
      _decodedState(generation: 10, flags: 20, frameCount: 1, selectedFrame: 0),
    );
    final resolved = await tester.runAsync(() => unresolved!.completion);
    expect(resolved?.state.active, isFalse);
    expect(resolved?.state.playing, isFalse);
    await notifications.close();
  });

  test('large 256-frame transfer checkpoints only for FIFO credit', () async {
    final transfer = _transfer(<LitchcraftAnimationFrame>[
      for (var index = 0; index < 256; index++) _frame(index, changes: 10),
    ]);
    final notifications = StreamController<LitchcraftV2Packet>.broadcast(
      sync: true,
    );
    final operations = <int>[];
    final client = LitchcraftFrameStoreClient(
      packets: notifications.stream,
      writeControl: (bytes) async {
        operations.add(bytes[2]);
        if (bytes[2] == LitchcraftFrameStoreProtocol.storeCheckpoint) {
          notifications.add(_checkpointReceipt(bytes));
        } else if (bytes[2] == LitchcraftFrameStoreProtocol.storeCommit) {
          notifications.add(_decodedState(generation: 10));
        }
      },
      writeBulk: (bytes) async {
        operations.add(bytes[2]);
      },
      requestReceipt: (_) async {},
      monotonicMilliseconds: () => 0,
    );
    final result = await client.send(transfer, maximumPacketBytes: 244);

    expect(result.bodyBytes, 26392);
    expect(result.chunkPackets, 115);
    expect(result.checkpoints, 3);
    expect(result.writes, 120);
    expect(
      operations.where(
        (operation) =>
            operation == LitchcraftFrameStoreProtocol.storeCheckpoint,
      ),
      hasLength(3),
    );
    expect(operations.last, LitchcraftFrameStoreProtocol.storeCommit);
    await notifications.close();
  });

  test(
    'shared ATT495 transfer streams across cumulative Object Progress',
    () async {
      final transfer = _transfer(<LitchcraftAnimationFrame>[
        for (var index = 0; index < 256; index++) _frame(index, changes: 10),
      ]);
      final notifications = StreamController<LitchcraftV2Packet>.broadcast(
        sync: true,
      );
      final operations = <int>[];
      final batchSizes = <int>[];
      final client = LitchcraftFrameStoreClient(
        packets: notifications.stream,
        writeControl: (bytes) async {
          operations.add(bytes[2]);
          if (bytes[2] == LitchcraftFrameStoreProtocol.storeCheckpoint) {
            notifications.add(_checkpointReceipt(bytes));
          } else if (bytes[2] == LitchcraftFrameStoreProtocol.storeCommit) {
            notifications.add(_decodedState(generation: 10));
          }
        },
        writeBulk: (bytes) async => operations.add(bytes[2]),
        writeBulkBatch: (packets) async {
          batchSizes.add(packets.length);
          operations.addAll(packets.map((bytes) => bytes[2]));
        },
        requestReceipt: (_) async {},
        monotonicMilliseconds: () => 0,
      );
      final result = await client.send(transfer, maximumPacketBytes: 481);

      expect(result.bodyBytes, 26392);
      expect(result.chunkPackets, 57);
      expect(result.checkpoints, 0);
      expect(result.writes, 59);
      expect(batchSizes, isNotEmpty);
      expect(batchSizes, everyElement(lessThanOrEqualTo(54)));
      expect(
        batchSizes,
        contains(LitchcraftFrameStoreClient.maximumNativeBatchPackets),
      );
      expect(
        operations.where(
          (operation) =>
              operation == LitchcraftFrameStoreProtocol.storeCheckpoint,
        ),
        hasLength(0),
      );
      expect(operations.last, LitchcraftFrameStoreProtocol.storeCommit);
      await notifications.close();
    },
  );

  test(
    'shared ATT244 transfer still keeps Commit as the durable barrier',
    () async {
      final transfer = _transfer(<LitchcraftAnimationFrame>[
        for (var index = 0; index < 256; index++) _frame(index, changes: 10),
      ]);
      final notifications = StreamController<LitchcraftV2Packet>.broadcast(
        sync: true,
      );
      final operations = <int>[];
      final client = LitchcraftFrameStoreClient(
        packets: notifications.stream,
        writeControl: (bytes) async {
          operations.add(bytes[2]);
          if (bytes[2] == LitchcraftFrameStoreProtocol.storeCommit) {
            notifications.add(_decodedState(generation: 10));
          }
        },
        writeBulk: (bytes) async => operations.add(bytes[2]),
        writeBulkBatch: (packets) async {
          operations.addAll(packets.map((bytes) => bytes[2]));
        },
        requestReceipt: (_) async {},
        monotonicMilliseconds: () => 0,
      );

      final result = await client.send(transfer, maximumPacketBytes: 244);

      expect(result.checkpoints, 0);
      expect(
        operations.where(
          (operation) =>
              operation == LitchcraftFrameStoreProtocol.storeCheckpoint,
        ),
        isEmpty,
      );
      expect(operations.last, LitchcraftFrameStoreProtocol.storeCommit);
      await notifications.close();
    },
  );

  test(
    'shared ATT244 max-density 256-frame transfer keeps one final barrier',
    () async {
      final transfer = _transfer(<LitchcraftAnimationFrame>[
        for (var index = 0; index < 256; index++) _frame(index),
      ]);
      final notifications = StreamController<LitchcraftV2Packet>.broadcast(
        sync: true,
      );
      final operations = <int>[];
      final batchSizes = <int>[];
      final client = LitchcraftFrameStoreClient(
        packets: notifications.stream,
        writeControl: (bytes) async {
          operations.add(bytes[2]);
          if (bytes[2] == LitchcraftFrameStoreProtocol.storeCommit) {
            notifications.add(_decodedState(generation: 10));
          }
        },
        writeBulk: (bytes) async => operations.add(bytes[2]),
        writeBulkBatch: (packets) async {
          batchSizes.add(packets.length);
          operations.addAll(packets.map((bytes) => bytes[2]));
        },
        requestReceipt: (_) async {},
        monotonicMilliseconds: () => 0,
      );

      final result = await client.send(transfer, maximumPacketBytes: 230);

      expect(result.bodyBytes, 289024);
      expect(result.chunkPackets, 1332);
      expect(result.checkpoints, 0);
      expect(result.writes, 1334);
      final fullSemanticWindows =
          result.chunkPackets ~/ LitchcraftFrameStoreClient.highMtuWindowChunks;
      final tailPackets =
          result.chunkPackets % LitchcraftFrameStoreClient.highMtuWindowChunks;
      final expectedBatchCount =
          fullSemanticWindows *
              ((LitchcraftFrameStoreClient.highMtuWindowChunks +
                      LitchcraftFrameStoreClient.maximumNativeBatchPackets -
                      1) ~/
                  LitchcraftFrameStoreClient.maximumNativeBatchPackets) +
          (tailPackets == 0
              ? 0
              : (tailPackets +
                        LitchcraftFrameStoreClient.maximumNativeBatchPackets -
                        1) ~/
                    LitchcraftFrameStoreClient.maximumNativeBatchPackets);
      expect(batchSizes, hasLength(expectedBatchCount));
      expect(
        batchSizes,
        everyElement(
          lessThanOrEqualTo(
            LitchcraftFrameStoreClient.maximumNativeBatchPackets,
          ),
        ),
      );
      expect(batchSizes, everyElement(greaterThan(0)));
      expect(
        operations.where(
          (operation) =>
              operation == LitchcraftFrameStoreProtocol.storeCheckpoint,
        ),
        isEmpty,
      );
      expect(operations.first, LitchcraftFrameStoreProtocol.storeBegin);
      expect(operations.last, LitchcraftFrameStoreProtocol.storeCommit);
      await notifications.close();
    },
  );

  test(
    'cold zero-session Inspect adopts the board projection session',
    () async {
      final notifications = StreamController<LitchcraftV2Packet>.broadcast(
        sync: true,
      );
      final client = LitchcraftFrameStoreClient(
        packets: notifications.stream,
        writeControl: (bytes) async {
          expect(
            ByteData.sublistView(
              Uint8List.fromList(bytes),
            ).getUint32(3, Endian.little),
            0,
          );
          notifications.add(
            LitchcraftV2Packet.decode(
              _inspectStatePacketWithResidents(
                session: 0xabcdef01,
                request: 71,
              ),
            )!,
          );
        },
        writeBulk: (_) async {},
        requestReceipt: (_) async {},
        monotonicMilliseconds: () => 0,
      );

      final state = await client.inspect(session: 0, request: 71);

      expect(state.session, 0xabcdef01);
      await notifications.close();
    },
  );

  test(
    '498-byte shared Store receipts reconstruct a predecessor-delta document',
    () async {
      final frames = <LitchcraftAnimationFrame>[
        _frame(1),
        LitchcraftAnimationFrame(emitters: _frame(1).emitters, durationMs: 52),
      ];
      final body = _snapshotBody(frames, independent: true);
      expect(body.length, greaterThan(438));
      expect(body.length, greaterThan(_snapshotBody(frames).length));
      final directory = await Directory.systemTemp.createTemp(
        'litch_snapshot_trace_',
      );
      final trace = await AppTraceRecorder.open(directory);
      addTearDown(() async {
        await trace.close();
        await directory.delete(recursive: true);
      });
      var now = 100;
      final notifications = StreamController<LitchcraftV2Packet>.broadcast(
        sync: true,
      );
      var nextRequest = 80;
      final client = LitchcraftFrameStoreClient(
        packets: notifications.stream,
        writeControl: (bytes) async {
          now = 400;
          expect(bytes[2], LitchcraftFrameStoreProtocol.storeSnapshot);
          final data = ByteData.sublistView(Uint8List.fromList(bytes));
          for (var cursor = 0; cursor < body.length; cursor += 438) {
            notifications.add(
              _snapshotPacket(
                body: body,
                request: data.getUint16(7, Endian.little),
                cursor: cursor,
                chunkBytes: 438,
                throughSharedObject: true,
              ),
            );
          }
        },
        writeBulk: (_) async {},
        requestReceipt: (_) async {},
        monotonicMilliseconds: () => now,
        appTrace: trace,
      );

      final snapshot = await client.snapshot(
        session: 0x12345678,
        generation: 9,
        allocateRequest: () => nextRequest++,
      );

      expect(snapshot.generation, 9);
      expect(snapshot.selectedFrame, 1);
      expect(snapshot.document.frames, hasLength(2));
      expect(snapshot.document.playbackOnEntry, isTrue);
      expect(snapshot.document.sparkleSeed, 7);
      expect(snapshot.document.frames[0].durationMs, frames[0].durationMs);
      expect(snapshot.document.frames[1].durationMs, frames[1].durationMs);
      expect(
        snapshot.document.frames[1].emitters.map((emitter) => emitter.color),
        frames[1].emitters.map((emitter) => emitter.color),
      );
      expect(nextRequest, 81);
      final persisted = (await trace.snapshot()).jsonl
          .split('\n')
          .where((line) => line.isNotEmpty)
          .map((line) => jsonDecode(line) as Map<String, Object?>)
          .singleWhere(
            (record) =>
                record['event'] == 'litchcraft.frame_store_snapshot_read',
          );
      final expected = {
        'read_kind': 'saved_body',
        'session': 0x12345678,
        'generation': 9,
        'request_id': 80,
        'frames': 2,
        'body_bytes': body.length,
        'body_crc32': litchcraftAnimationCrc32(body),
        'chunks': (body.length / 438).ceil(),
        'cache_hits': 0,
        'elapsed_ms': 300,
      };
      for (final entry in expected.entries) {
        expect(persisted[entry.key], entry.value, reason: entry.key);
      }
      expect(persisted['fields_omitted'], isNull);
      await notifications.close();
    },
  );

  test(
    'capability 7 Inspect refuses a State without resident listing',
    () async {
      final notifications = StreamController<LitchcraftV2Packet>.broadcast(
        sync: true,
      );
      final client = LitchcraftFrameStoreClient(
        packets: notifications.stream,
        writeControl: (bytes) async {
          expect(
            bytes.last,
            LitchcraftFrameStoreProtocol.residentListingCapability,
          );
          notifications.add(
            LitchcraftV2Packet.decode(
              _statePacket(session: 0xabcdef01, request: 71),
            )!,
          );
        },
        writeBulk: (_) async {},
        requestReceipt: (_) async {},
        monotonicMilliseconds: () => 0,
      );

      await expectLater(
        client.inspect(session: 0, request: 71),
        throwsA(isA<FormatException>()),
      );
      await notifications.close();
    },
  );

  test('one request streams and verifies 256 frames', () async {
    final frames = <LitchcraftAnimationFrame>[
      for (var index = 0; index < 256; index++) _frame(index),
    ];
    final body = _snapshotBody(frames);
    final notifications = StreamController<LitchcraftV2Packet>.broadcast(
      sync: true,
    );
    var nextRequest = 80;
    var controlWrites = 0;
    final progress = <int>[];
    final client = LitchcraftFrameStoreClient(
      packets: notifications.stream,
      writeControl: (bytes) async {
        controlWrites++;
        if (controlWrites != 1) {
          throw StateError('read stream issued a second request');
        }
        final bodyCrc32 = litchcraftAnimationCrc32(body);
        final data = ByteData.sublistView(Uint8List.fromList(bytes));
        for (var cursor = 0; cursor < body.length; cursor += 170) {
          notifications.add(
            _snapshotPacket(
              body: body,
              request: data.getUint16(7, Endian.little),
              cursor: cursor,
              frameCount: frames.length,
              bodyCrc32: bodyCrc32,
            ),
          );
        }
      },
      writeBulk: (_) async {},
      writeBulkBatch: (_) async => throw StateError('unexpected read batch'),
      requestReceipt: (_) async {},
      monotonicMilliseconds: () => 0,
    );

    final snapshot = await client.snapshot(
      session: 0x12345678,
      generation: 9,
      allocateRequest: () => nextRequest++,
      onProgress: (received, total) {
        expect(total, body.length);
        progress.add(received);
      },
    );

    expect(snapshot.document.frames, hasLength(frames.length));
    expect(progress, isNotEmpty);
    expect(progress.last, body.length);
    expect(progress, orderedEquals(progress.toList()..sort()));
    for (var index = 0; index < frames.length; index++) {
      expect(
        snapshot.document.frames[index].durationMs,
        frames[index].durationMs,
      );
      expect(
        snapshot.document.frames[index].emitters.map(
          (emitter) => emitter.color,
        ),
        frames[index].emitters.map((emitter) => emitter.color),
      );
    }
    expect(controlWrites, 1);
    expect(nextRequest, 81);
    await notifications.close();
  });

  test('changed cursor aborts a partial board read before adoption', () async {
    final trace = _SnapshotTrace();
    final frames = <LitchcraftAnimationFrame>[
      for (var index = 0; index < 8; index++) _frame(index),
    ];
    final body = _snapshotBody(frames);
    final notifications = StreamController<LitchcraftV2Packet>.broadcast(
      sync: true,
    );
    final progress = <int>[];
    var writes = 0;
    final client = LitchcraftFrameStoreClient(
      packets: notifications.stream,
      writeControl: (bytes) async {
        writes++;
        final request = ByteData.sublistView(
          Uint8List.fromList(bytes),
        ).getUint16(7, Endian.little);
        notifications.add(
          _snapshotPacket(
            body: body,
            request: request,
            cursor: 0,
            frameCount: frames.length,
          ),
        );
        notifications.add(
          _snapshotPacket(
            body: body,
            request: request,
            cursor: 171,
            frameCount: frames.length,
          ),
        );
      },
      writeBulk: (_) async {},
      requestReceipt: (_) async {},
      monotonicMilliseconds: () => 0,
      appTrace: trace,
    );

    await expectLater(
      client.snapshot(
        session: 0x12345678,
        generation: 9,
        allocateRequest: () => 80,
        onProgress: (received, _) => progress.add(received),
      ),
      throwsFormatException,
    );
    expect(writes, 1);
    expect(progress, [170]);
    expect(
      trace.reads,
      isEmpty,
      reason: 'partial bytes are not a successful body read',
    );
    await notifications.close();
  });

  test('lost reverse chunk resumes from the last verified cursor', () async {
    final frames = <LitchcraftAnimationFrame>[
      for (var index = 0; index < 8; index++) _frame(index),
    ];
    final body = _snapshotBody(frames);
    final notifications = StreamController<LitchcraftV2Packet>.broadcast(
      sync: true,
    );
    var nextRequest = 80;
    final cursors = <int>[];
    final retries = <(int, int)>[];
    final client = LitchcraftFrameStoreClient(
      packets: notifications.stream,
      writeControl: (bytes) async {
        final data = ByteData.sublistView(Uint8List.fromList(bytes));
        final request = data.getUint16(7, Endian.little);
        final cursor = data.getUint32(13, Endian.little);
        cursors.add(cursor);
        if (cursor == 0) {
          notifications.add(
            _snapshotPacket(
              body: body,
              request: request,
              cursor: 0,
              frameCount: frames.length,
            ),
          );
          return;
        }
        notifications.add(
          _snapshotPacket(
            body: body,
            request: request - 1,
            cursor: cursor,
            frameCount: frames.length,
          ),
        );
        for (var offset = cursor; offset < body.length; offset += 170) {
          notifications.add(
            _snapshotPacket(
              body: body,
              request: request,
              cursor: offset,
              frameCount: frames.length,
            ),
          );
        }
      },
      writeBulk: (_) async {},
      responseTimeout: const Duration(milliseconds: 10),
      requestReceipt: (_) async {},
      monotonicMilliseconds: () => 0,
    );

    final snapshot = await client.snapshot(
      session: 0x12345678,
      generation: 9,
      allocateRequest: () => nextRequest++,
      onRetry: (cursor, attempt) => retries.add((cursor, attempt)),
    );
    expect(cursors, [0, 170]);
    expect(retries, [(170, 1)]);
    expect(nextRequest, 82);
    expect(snapshot.document.frames.length, frames.length);
    await notifications.close();
  });

  for (final phase in <String>['admission', 'response']) {
    test(
      'snapshot local cancellation retires $phase without upload Cancel',
      () async {
        var retired = false;
        final notifications = StreamController<LitchcraftV2Packet>.broadcast(
          onCancel: () => retired = true,
        );
        final releaseWrite = Completer<void>();
        final cancellation = Completer<void>();
        final operations = <int>[];
        final client = LitchcraftFrameStoreClient(
          packets: notifications.stream,
          writeControl: (bytes) async {
            operations.add(bytes[2]);
            if (phase == 'admission') await releaseWrite.future;
          },
          writeBulk: (_) async {},
          responseTimeout: const Duration(milliseconds: 20),
          requestReceipt: (_) async {},
          monotonicMilliseconds: () => 0,
        );
        final read = client.snapshot(
          session: 0x12345678,
          generation: 9,
          allocateRequest: () => 80,
          cancellationFuture: cancellation.future,
        );
        final failure = expectLater(
          read,
          throwsA(isA<LitchcraftTransferCancelled>()),
        );
        await Future<void>.delayed(Duration.zero);
        expect(notifications.hasListener, isTrue);
        cancellation.complete();
        await failure;
        expect(retired, isTrue);
        expect(notifications.hasListener, isFalse);
        releaseWrite.complete();
        await Future<void>.delayed(const Duration(milliseconds: 30));
        expect(operations, <int>[LitchcraftFrameStoreProtocol.storeSnapshot]);
        await notifications.close();
      },
    );
  }

  test(
    'snapshot admission cannot leave artwork read pending forever',
    () async {
      final notifications = StreamController<LitchcraftV2Packet>.broadcast();
      final blockedWrite = Completer<void>();
      final client = LitchcraftFrameStoreClient(
        packets: notifications.stream,
        writeControl: (_) => blockedWrite.future,
        writeBulk: (_) async {},
        responseTimeout: const Duration(milliseconds: 10),
        requestReceipt: (_) async {},
        monotonicMilliseconds: () => 0,
      );

      await expectLater(
        client
            .snapshot(
              session: 0x12345678,
              generation: 9,
              allocateRequest: () => 80,
            )
            .timeout(const Duration(seconds: 1)),
        throwsA(
          isA<TimeoutException>().having(
            (error) => error.message,
            'phase and cursor',
            contains('admission stalled: cursor=0'),
          ),
        ),
      );
      await notifications.close();
    },
  );
}
