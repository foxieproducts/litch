import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/litchcraft_animation_codec.dart';
import 'package:litch_mobile/src/models/litchcraft_document_session.dart';
import 'package:litch_mobile/src/services/litchcraft_protocol.dart';

import 'fixtures/litchcraft_v2_functional_peer.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';

class _WindowPeer extends LitchcraftV2FunctionalPeer {
  bool supportsWindows = true;
  Future<void>? checkpointGate;

  @override
  void respond(List<int> bytes) {
    if (bytes[0] == LitchcraftV2Protocol.magic &&
        (bytes[2] == LitchcraftV2Protocol.streamCheckpoint ||
            bytes[2] == LitchcraftV2Protocol.inspect &&
                bytes.length == 10 &&
                bytes[9] == 3)) {
      writes.add(List.of(bytes));
      final request = bytes[7] | bytes[8] << 8;
      if (bytes[2] == LitchcraftV2Protocol.inspect) {
        state(request, status: supportsWindows ? 0 : 3);
      } else {
        final cursor = bytes[11] | bytes[12] << 8;
        gatedState(
          request,
          checkpointGate,
          status: cursor == staged.length ? 0 : 5,
        );
      }
      return;
    }
    super.respond(bytes);
  }
}

Future<void> _finish(WidgetTester tester, Future<void> work) async {
  var done = false;
  Object? failure;
  work.then(
    (_) => done = true,
    onError: (Object e) {
      failure = e;
      done = true;
    },
  );
  for (var i = 0; i < 5000 && !done; i++) {
    await tester.pump(const Duration(milliseconds: 1));
  }
  expect(done, isTrue);
  if (failure != null) throw failure!;
}

void main() {
  testWidgets(
    'domain stream without window support uses acknowledged packets at ATT182',
    (tester) async {
      final peer = _WindowPeer()..supportsWindows = false;
      final link = LitchcraftLink(
        packets: peer.packets.stream,
        write: (bytes) async => peer.respond(bytes),
        writeBulkDocumentPacket: (bytes) async => peer.respond(bytes),
        onPacket: (_) {},
        onFailure: (_) {},
      );
      final client = LitchcraftV2DocumentClient(link);
      final encoded = encodeLitchcraftAnimationDocument(peer.document);
      for (var i = 0; i < 2; i++) {
        await _finish(
          tester,
          client
              .replace(
                session: 42,
                baseRevision: peer.revision,
                document: encoded.bytes,
                crc32: encoded.crc32,
                maximumPacketBytes: 182,
              )
              .then((_) {}),
        );
      }
      expect(
        peer.writes.where((p) => p[2] == 1 && p.length == 10 && p[9] == 3),
        hasLength(1),
      );
      expect(peer.writes.where((p) => p[2] == 15 || p[2] == 17), isEmpty);
      expect(peer.writes.where((p) => p[2] == 11), hasLength(2));
      expect(
        encodeLitchcraftAnimationDocument(peer.document).bytes,
        encoded.bytes,
      );
      await tester.runAsync(() async {
        await link.dispose();
        await peer.close();
      });
    },
  );

  testWidgets(
    'missing checkpoint ACK times out without publishing staged bytes',
    (tester) async {
      final peer = _WindowPeer();
      final gate = Completer<void>();
      peer.checkpointGate = gate.future;
      final link = LitchcraftLink(
        packets: peer.packets.stream,
        write: (bytes) async => peer.respond(bytes),
        writeBulkDocumentPacket: (bytes) async => peer.respond(bytes),
        timeout: const Duration(milliseconds: 20),
        onPacket: (_) {},
        onFailure: (_) {},
      );
      final client = LitchcraftV2DocumentClient(link);
      final before = encodeLitchcraftAnimationDocument(peer.document);
      final edited = LitchcraftDocumentSession(peer.document)
        ..setSelectedFrameDuration(2100);
      final encoded = encodeLitchcraftAnimationDocument(edited.document);
      Object? failure;
      await _finish(
        tester,
        client
            .replace(
              session: 42,
              baseRevision: peer.revision,
              document: encoded.bytes,
              crc32: encoded.crc32,
              maximumPacketBytes: 20,
            )
            .then(
              (_) {},
              onError: (Object e) {
                failure = e;
              },
            ),
      );
      expect(failure, isA<TimeoutException>());
      final checkpoints = peer.writes.where((p) => p[2] == 17).toList();
      expect(checkpoints, hasLength(3));
      expect(
        checkpoints.every((p) => listEquals(p, checkpoints.first)),
        isTrue,
      );
      expect(peer.writes.where((p) => p[2] == 16), isEmpty);
      expect(
        encodeLitchcraftAnimationDocument(peer.document).bytes,
        before.bytes,
      );
      gate.complete();
      await tester.pump();
      await tester.runAsync(() async {
        await link.dispose();
        await peer.close();
      });
    },
  );

  for (final payload in [20, 182, 244]) {
    testWidgets('eight frames stream in bounded windows at ATT payload $payload', (
      tester,
    ) async {
      debugDefaultTargetPlatformOverride = TargetPlatform.iOS;
      final peer = _WindowPeer()..nativeMtuResult = payload;
      final ble = LitchcraftV2FunctionalService(peer);
      var negotiated = 0;
      await _finish(
        tester,
        ble
            .litchcraftPacketPayloadBytes(
              'board',
              connectionSessionToken: ble.connectionSessionToken,
            )
            .then((n) => negotiated = n),
      );
      expect(negotiated, payload);
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
      final edited = LitchcraftDocumentSession(peer.document);
      while (edited.document.frames.length < 8) {
        edited.addFrame();
      }
      edited.setSelectedFrameDuration(2100);
      final encoded = encodeLitchcraftAnimationDocument(edited.document);
      final gate = Completer<void>();
      if (payload < 244) peer.checkpointGate = gate.future;
      peer.beforeDomainWork = () =>
          Future<void>.delayed(const Duration(milliseconds: 1));
      final delivery = client
          .replace(
            session: 42,
            baseRevision: peer.revision,
            document: encoded.bytes,
            crc32: encoded.crc32,
            maximumPacketBytes: payload,
          )
          .then((_) {});
      if (payload < 244) {
        for (var i = 0; i < 2000 && !peer.writes.any((p) => p[2] == 17); i++) {
          await tester.pump(const Duration(milliseconds: 1));
        }
        await tester.pump(const Duration(milliseconds: 10));
        expect(
          peer.writes.where((p) => p[2] == 15),
          hasLength(32),
          reason:
              'native admission cannot release a second window before the board cursor ACK',
        );
        expect(
          peer.document.frames.length,
          2,
          reason: 'staging is not live artwork',
        );
        gate.complete();
      }
      await _finish(tester, delivery);
      expect(
        peer.writes.where((p) => p[2] == 10),
        isEmpty,
        reason:
            'negotiated payload must not select per-chunk application round trips',
      );
      final chunks = (encoded.bytes.length / (payload - 13)).ceil();
      final checkpoints = payload == 244 ? 0 : (chunks - 1) ~/ 32;
      expect(peer.writes.where((p) => p[2] == 15), hasLength(chunks));
      expect(peer.writes.where((p) => p[2] == 17), hasLength(checkpoints));
      expect(
        peer.sharedWrites.where(
          (packet) =>
              packet.kind == ConnectionSessionKind.object &&
              packet.code == 0x40,
        ),
        hasLength(greaterThanOrEqualTo(chunks + 3 + checkpoints)),
      );
      expect(peer.writes.every((p) => p.length <= payload), isTrue);
      final restored = encodeLitchcraftAnimationDocument(peer.document);
      expect(restored.bytes, encoded.bytes);
      expect(restored.crc32, encoded.crc32);
      await tester.runAsync(() async {
        await link.dispose();
        await ble.dispose();
        await peer.close();
      });
      debugDefaultTargetPlatformOverride = null;
    });
  }
}
