import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';
import 'package:litch_mobile/src/services/connection_session.dart';

void main() {
  test('shared firmware/app packet vectors fit the minimum ATT payload', () {
    final rows = File('../test/fixtures/connection_session_v4.csv')
        .readAsLinesSync()
        .where((line) => line.isNotEmpty && !line.startsWith('#'));
    expect(rows.length, 53);
    var sawOrderedMode = false;
    for (final row in rows) {
      final fields = row.split(',');
      final hex = fields[2];
      final bytes = Uint8List.fromList([
        for (var i = 0; i < hex.length; i += 2)
          int.parse(hex.substring(i, i + 2), radix: 16),
      ]);
      final packet = ConnectionSessionCodec.decode(bytes, packetLimit: 20);
      expect(packet != null, fields[1] == '1', reason: fields[0]);
      if (packet == null) continue;
      if (fields[0] == 'identity_ordered_mode') {
        expect(packet.kind, ConnectionSessionKind.control);
        expect(packet.code, ConnectionSessionControl.identity);
        expect(
          ByteData.sublistView(packet.body).getUint16(8, Endian.little) &
              ConnectionSession.orderedModeFeature,
          ConnectionSession.orderedModeFeature,
        );
        sawOrderedMode = true;
      }
      expect(ConnectionSessionCodec.encode(packet), bytes, reason: fields[0]);
      if (packet.kind == ConnectionSessionKind.control) {
        for (final size in {9, bytes.length - 1}) {
          expect(
            ConnectionSessionCodec.decode(
              Uint8List.sublistView(bytes, 0, size),
            ),
            isNull,
            reason: '${fields[0]} truncated to $size',
          );
        }
      }
    }
    expect(sawOrderedMode, isTrue);
  });

  test(
    'bulk uses actual payload bound and cannot truncate caller identities',
    () {
      ConnectionSessionPacket packet({int token = 12, int sequence = 2}) =>
          ConnectionSessionPacket(
            kind: ConnectionSessionKind.object,
            code: 37,
            token: token,
            sequence: sequence,
            body: Uint8List.fromList(
              List.generate(502, (index) => index & 255),
            ),
          );
      // Bulk64 and Ordered16 are wire limits, independent of a peer's
      // smaller selected queue capacity and the app's default Ordered8.
      for (final lane in [0, 1]) {
        final maximum = lane == 0 ? 16 : 64;
        final body = Uint8List.fromList([1, 0, lane, maximum, 10, 0, 40, 0]);
        final open = ConnectionSessionPacket(
          kind: ConnectionSessionKind.control,
          code: ConnectionSessionControl.open,
          token: 1,
          sequence: 1,
          body: body,
        );
        expect(
          ConnectionSessionCodec.decode(ConnectionSessionCodec.encode(open)),
          isNotNull,
        );
        body[3] = maximum + 1;
        expect(
          () => ConnectionSessionCodec.encode(open),
          throwsFormatException,
        );
      }
      final progressBody = Uint8List(10);
      ByteData.sublistView(progressBody).setUint32(4, 64, Endian.little);
      final progress = ConnectionSessionPacket(
        kind: ConnectionSessionKind.control,
        code: ConnectionSessionControl.progress,
        token: 3,
        sequence: 0,
        body: progressBody,
      );
      expect(
        ConnectionSessionCodec.decode(ConnectionSessionCodec.encode(progress)),
        isNotNull,
      );
      ByteData.sublistView(progressBody).setUint32(4, 65, Endian.little);
      expect(
        () => ConnectionSessionCodec.encode(progress),
        throwsFormatException,
      );
      expect(
        () => ConnectionSession(
          packets: const Stream<List<int>>.empty(),
          send: (_) async {},
          clientNonce: 1,
          orderedCapacity: 17,
        ),
        throwsArgumentError,
      );
      final encoded = ConnectionSessionCodec.encode(packet());
      expect(encoded.length, 512);
      expect(ConnectionSessionCodec.decode(encoded, packetLimit: 244), isNull);
      expect(ConnectionSessionCodec.decode(encoded)!.body, packet().body);
      expect(ConnectionSessionCodec.decode(Uint8List(513)), isNull);
      expect(ConnectionSessionCodec.decode(encoded, packetLimit: 19), isNull);
      for (final token in [-1, 0, 0x100000000]) {
        expect(
          () => ConnectionSessionCodec.encode(packet(token: token)),
          throwsFormatException,
        );
      }
      for (final sequence in [-1, 0, 0xffffffff, 0x100000000]) {
        expect(
          () => ConnectionSessionCodec.encode(packet(sequence: sequence)),
          throwsFormatException,
        );
      }
    },
  );
}
