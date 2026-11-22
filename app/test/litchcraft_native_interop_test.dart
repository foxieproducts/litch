import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/services/litchcraft_protocol.dart';

/// An integration gate can set LITCHCRAFT_NATIVE_RUNNER to the freshly built
/// native aggregate. This consumes real C++ wire bytes, not a Dart fake encoder.
void main() {
  final runner = Platform.environment['LITCHCRAFT_NATIVE_RUNNER'];
  test(
    'C++ canvas stream decodes and restores asymmetric artwork in Dart',
    () async {
      final result = await Process.run(
        runner!,
        const <String>[],
        workingDirectory: Directory.current.parent.path,
        environment: {
          'LITCH_NATIVE_TEST_FILTER': 'test_litchcraft',
          'LITCHCRAFT_WIRE_CAPTURE': '1',
        },
      ).timeout(const Duration(seconds: 60));
      expect(result.exitCode, 0, reason: '${result.stdout}\n${result.stderr}');
      final lines = (result.stdout as String)
          .split('\n')
          .where((line) => line.startsWith('[LITCHCRAFT_WIRE] '));
      expect(
        lines,
        isNotEmpty,
        reason: 'Native runner must include the wire capture journey',
      );
      final packets = <LitchcraftPacket>[];
      for (final line in lines) {
        final hex = line
            .substring('[LITCHCRAFT_WIRE] '.length)
            .replaceAll(' ', '')
            .trim();
        expect(hex.length.isEven, isTrue);
        final bytes = [
          for (var i = 0; i < hex.length; i += 2)
            int.parse(hex.substring(i, i + 2), radix: 16),
        ];
        final packet = LitchcraftPacket.decode(bytes);
        expect(packet, isNotNull, reason: 'Actual C++ frame rejected: $line');
        packets.add(packet!);
      }
      expect(
        packets
            .where((p) => p.event == LitchcraftProtocol.state)
            .any((p) => p.payload[3] >= 2),
        isTrue,
        reason:
            'Counts, not booleans, must remain interoperable after two strokes',
      );
      final begin = packets.lastWhere(
        (p) => p.event == LitchcraftProtocol.snapshotBegin,
      );
      final chunks = packets.where(
        (p) =>
            p.event == LitchcraftProtocol.snapshotChunk &&
            p.requestId == begin.requestId &&
            p.session == begin.session &&
            p.revision == begin.revision,
      );
      final colors = List<int?>.filled(256, null);
      for (final chunk in chunks) {
        final offset = chunk.payload[0] | chunk.payload[1] << 8;
        for (var index = 2; index < chunk.payload.length; index += 3) {
          colors[offset + (index - 2) ~/ 3] = LitchcraftProtocol.readRgb(
            chunk.payload,
            index,
          );
        }
      }
      expect(colors, everyElement(isNotNull));
      expect(colors.take(4), everyElement(0x123456));
      expect(colors.skip(252), everyElement(0x123456));
      expect(colors.skip(4).take(248), everyElement(0));
      expect(
        packets.any(
          (p) =>
              p.event == LitchcraftProtocol.snapshotEnd &&
              p.requestId == begin.requestId &&
              p.session == begin.session &&
              p.revision == begin.revision,
        ),
        isTrue,
      );
    },
    skip: runner == null
        ? 'Set LITCHCRAFT_NATIVE_RUNNER for the native/app integration gate'
        : false,
  );
}
