import 'dart:async';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_animation_codec.dart';
import 'package:litch_mobile/src/models/litchcraft_wire_codec.dart';
import 'package:litch_mobile/src/services/litchcraft_frame_store_protocol.dart';
import 'package:litch_mobile/src/services/litchcraft_protocol.dart';

Uint8List _livePacket({
  required bool layout,
  int frameCount = 256,
  Uint8List? body,
  int cursor = 0,
  int request = 1,
  int? selectedFrame,
  int revision = 13,
  int epoch = 2,
  int scope = 1,
}) {
  body ??= Uint8List(layout ? frameCount * 6 : 9);
  final count = (body.length - cursor).clamp(0, 170);
  final packet = Uint8List(78 + count);
  final data = ByteData.sublistView(packet);
  packet[0] = LitchcraftV2Protocol.magic;
  packet[1] = LitchcraftV2Protocol.version;
  packet[2] = LitchcraftV2Protocol.frameStoreSnapshotChunk;
  data.setUint32(3, 1, Endian.little);
  data.setUint32(7, 9, Endian.little);
  data.setUint16(11, request, Endian.little);
  packet[14] = 1 | 4 | 64 | (layout ? 128 : 32);
  packet[15] = 3;
  data.setUint16(16, frameCount, Endian.little);
  data.setUint16(18, selectedFrame ?? frameCount - 1, Endian.little);
  data.setUint32(20, body.length, Endian.little);
  data.setUint32(24, litchcraftAnimationCrc32(body), Endian.little);
  data.setUint32(28, cursor, Endian.little);
  data.setUint32(32, 7, Endian.little);
  data.setUint32(60, revision, Endian.little);
  data.setUint32(64, epoch, Endian.little);
  data.setUint32(68, 12, Endian.little);
  packet[72] = scope;
  data.setUint16(73, frameCount - 1, Endian.little);
  packet.setRange(78, packet.length, body, cursor);
  return packet;
}

LitchcraftAnimationFrame _frame(int seed) => LitchcraftAnimationFrame(
  durationMs: 1000 + seed,
  emitters: List.generate(
    256,
    (i) => LitchcraftAnimatedEmitter(
      color: (seed << 8) + i,
      effect: i % 2 == 0
          ? LitchcraftEffectStyle.breathe
          : LitchcraftEffectStyle.off,
      effectDurationMs: i % 2 == 0 ? 1600 : 0,
    ),
  ),
);

Uint8List _record(LitchcraftAnimationFrame frame) {
  final encoded = encodeLitchcraftWireFrame(previous: null, frame: frame);
  final result = Uint8List(6 + encoded.length);
  final data = ByteData.sublistView(result);
  data.setUint16(0, encoded.length, Endian.little);
  data.setUint32(2, litchcraftAnimationCrc32(encoded), Endian.little);
  result.setRange(6, result.length, encoded);
  return result;
}

Uint8List _layout(List<LitchcraftAnimationFrame> frames) {
  final result = Uint8List(frames.length * 6);
  final data = ByteData.sublistView(result);
  for (var i = 0; i < frames.length; i++) {
    final record = _record(frames[i]);
    data.setUint16(i * 6, record.length, Endian.little);
    data.setUint32(i * 6 + 2, litchcraftAnimationCrc32(record), Endian.little);
  }
  return result;
}

({
  LitchcraftFrameStoreClient client,
  StreamController<LitchcraftV2Packet> packets,
  List<int> selectors,
})
_peer(List<LitchcraftAnimationFrame> frames, {String? fault}) {
  final packets = StreamController<LitchcraftV2Packet>.broadcast(sync: true);
  final selectors = <int>[];
  final client = LitchcraftFrameStoreClient(
    packets: packets.stream,
    writeControl: (request) async {
      expect(request, hasLength(23));
      final input = ByteData.sublistView(Uint8List.fromList(request));
      expect(input.getUint32(19, Endian.little), 13);
      final selector = input.getUint16(17, Endian.little);
      selectors.add(selector);
      final layout = selector == 0xfffe;
      final body = layout ? _layout(frames) : _record(frames[selector]);
      if (!layout && fault == 'changed record') body[body.length - 1] ^= 1;
      for (var cursor = 0; cursor < body.length; cursor += 170) {
        final raw = _livePacket(
          layout: layout,
          frameCount: frames.length,
          body: body,
          cursor: cursor,
          request: input.getUint16(7, Endian.little),
          selectedFrame: layout ? frames.length - 1 : selector,
          revision: !layout && fault == 'revision' ? 14 : 13,
          epoch: !layout && fault == 'epoch' ? 3 : 2,
          scope: !layout && fault == 'scope' ? 2 : 1,
        );
        if (layout && fault == 'body CRC' && cursor == 0) raw[24] ^= 1;
        if (fault == 'cursor' && cursor > 0) {
          ByteData.sublistView(raw).setUint32(28, 0, Endian.little);
        }
        packets.add(LitchcraftV2Packet.decode(raw)!);
      }
    },
    writeBulk: (_) async => fail('live read must never upload artwork'),
    requestReceipt: (_) async => fail('live read is not an upload'),
    monotonicMilliseconds: () => 0,
  );
  return (client: client, packets: packets, selectors: selectors);
}

void main() {
  for (final layout in [false, true]) {
    test('live Store ${layout ? 'layout' : 'frame'} is admitted', () {
      // Keep the response within the real decoder's 498-byte receipt bound.
      final raw = _livePacket(layout: layout, frameCount: 2);
      expect(LitchcraftV2Packet.decode(raw), isNotNull);
    });
  }

  test(
    'live request distinguishes layout and exact revision from saved reads',
    () {
      final live = LitchcraftFrameStoreProtocol.snapshotRequest(
        session: 1,
        request: 7,
        generation: 9,
        cursor: 0,
        frame: 0xfffe,
        liveRevision: 13,
      );
      expect(live, hasLength(23));
      expect(ByteData.sublistView(live).getUint16(17, Endian.little), 0xfffe);
      expect(ByteData.sublistView(live).getUint32(19, Endian.little), 13);
      expect(
        () => LitchcraftFrameStoreProtocol.snapshotRequest(
          session: 1,
          request: 7,
          generation: 9,
          cursor: 0,
          frame: 0xfffe,
        ),
        throwsArgumentError,
      );
    },
  );

  for (final fault in [
    'reserved',
    'zero epoch',
    'scope',
    'two modes',
    'missing live flag',
    'delta',
  ]) {
    test('live metadata rejects $fault before adoption', () {
      final raw = _livePacket(layout: true, frameCount: 2);
      switch (fault) {
        case 'reserved':
          raw[77] = 1;
        case 'zero epoch':
          ByteData.sublistView(raw).setUint32(64, 0, Endian.little);
        case 'scope':
          raw[72] = 4;
        case 'two modes':
          raw[14] |= 32;
        case 'missing live flag':
          raw[14] &= ~64;
        case 'delta':
          raw[14] |= 8;
      }
      expect(LitchcraftV2Packet.decode(raw), isNull);
    });
  }

  for (final missing in [
    <int>[],
    <int>[250],
    <int>[7, 250],
  ]) {
    test('256 live layout reads only unmatched frames $missing', () async {
      final frames = List.generate(256, _frame);
      final peer = _peer(frames);
      var heldRead = Completer<void>();
      var releaseRead = Completer<void>();
      final progress = <(int, int, int)>[];
      addTearDown(() {
        if (!releaseRead.isCompleted) releaseRead.complete();
      });
      final client = LitchcraftFrameStoreClient(
        packets: peer.packets.stream,
        writeControl: (bytes) async {
          final selector = ByteData.sublistView(
            Uint8List.fromList(bytes),
          ).getUint16(17, Endian.little);
          if (missing.isNotEmpty && selector == missing.last) {
            heldRead.complete();
            await releaseRead.future;
          }
          await peer.client.writeControl(bytes);
        },
        writeBulk: peer.client.writeBulk,
        requestReceipt: peer.client.requestReceipt,
        monotonicMilliseconds: () => 0,
      );
      var request = 0;
      final reading = client.liveSnapshot(
        session: 1,
        generation: 9,
        liveRevision: 13,
        allocateRequest: () => ++request,
        knownFrames: [
          for (var i = 0; i < 256; i++)
            if (!missing.contains(i)) frames[i],
        ],
        onProgress: (received, total, count) =>
            progress.add((received, total, count)),
      );
      final totalBytes =
          _layout(frames).length +
          frames.fold<int>(0, (total, frame) => total + _record(frame).length);
      if (missing.isNotEmpty) {
        await heldRead.future.timeout(const Duration(seconds: 3));
        expect(progress, isNotEmpty);
        expect(progress.last.$1, lessThan(totalBytes));
        expect(progress.last.$1, greaterThanOrEqualTo(_layout(frames).length));
        releaseRead.complete();
      }
      final snapshot = await reading;
      expect(progress.first, (_layout(frames).length, totalBytes, 256));
      expect(progress.last, (totalBytes, totalBytes, 256));
      expect(
        progress.every((value) => value.$2 == totalBytes && value.$3 == 256),
        isTrue,
      );
      expect(
        progress.map((value) => value.$1),
        orderedEquals(progress.map((value) => value.$1).toList()..sort()),
      );
      expect(peer.selectors, [0xfffe, ...missing]);
      expect(snapshot.liveRevision, 13);
      expect(snapshot.documentEpoch, 2);
      expect(snapshot.document.frames, hasLength(256));
      for (var i = 0; i < 256; i++) {
        final actual = snapshot.document.frames[i], expected = frames[i];
        expect(actual.durationMs, expected.durationMs);
        expect(
          actual.emitters.map((e) => (e.color, e.effect, e.effectDurationMs)),
          expected.emitters.map((e) => (e.color, e.effect, e.effectDurationMs)),
        );
        if (!missing.contains(i)) expect(identical(actual, expected), isTrue);
      }
      if (missing.isNotEmpty) {
        heldRead = Completer<void>();
        releaseRead = Completer<void>();
        final cancellation = Completer<void>();
        progress.clear();
        final cancelledRead = client.liveSnapshot(
          session: 1,
          generation: 9,
          liveRevision: 13,
          allocateRequest: () => ++request,
          knownFrames: [
            for (var i = 0; i < 256; i++)
              if (!missing.contains(i)) frames[i],
          ],
          cancellationFuture: cancellation.future,
          onProgress: (received, total, count) =>
              progress.add((received, total, count)),
        );
        final failed = expectLater(
          cancelledRead,
          throwsA(isA<LitchcraftTransferCancelled>()),
        );
        await heldRead.future.timeout(const Duration(seconds: 3));
        expect(progress, isNotEmpty);
        final beforeCancellation = progress.toList();
        cancellation.complete();
        await failed;
        releaseRead.complete();
        await Future<void>.delayed(Duration.zero);
        expect(progress, beforeCancellation);
      }
      await peer.packets.close();
    });
  }

  test(
    'structural reorder and aliases reuse known content at new indices',
    () async {
      final a = _frame(1), b = _frame(2), c = _frame(3);
      final peer = _peer([c, a, a, b]);
      var request = 0;
      final snapshot = await peer.client.liveSnapshot(
        session: 1,
        generation: 9,
        liveRevision: 13,
        allocateRequest: () => ++request,
        knownFrames: [a, b],
      );
      expect(peer.selectors, [0xfffe, 0]);
      expect(snapshot.document.frames.map((f) => f.durationMs), [
        1003,
        1001,
        1001,
        1002,
      ]);
      expect(identical(snapshot.document.frames[1], a), isTrue);
      expect(identical(snapshot.document.frames[2], a), isTrue);
      await peer.packets.close();
    },
  );

  for (final fault in [
    'revision',
    'epoch',
    'scope',
    'changed record',
    'body CRC',
    'cursor',
  ]) {
    test(
      'selective live read rejects $fault without returning a document',
      () async {
        final peer = _peer([_frame(1), _frame(2)], fault: fault);
        var request = 0;
        await expectLater(
          peer.client.liveSnapshot(
            session: 1,
            generation: 9,
            liveRevision: 13,
            allocateRequest: () => ++request,
            knownFrames: [],
          ),
          throwsFormatException,
        );
        await peer.packets.close();
      },
    );
  }
}
