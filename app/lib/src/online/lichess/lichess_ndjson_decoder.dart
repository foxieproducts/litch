import 'dart:convert';

import 'lichess_protocol.dart';

final class LichessNdjsonDecoder {
  const LichessNdjsonDecoder({this.maximumLineBytes = 64 * 1024})
    : assert(maximumLineBytes > 0);

  final int maximumLineBytes;

  Stream<Map<String, Object?>> decode(Stream<List<int>> chunks) async* {
    final List<int> line = <int>[];

    await for (final List<int> chunk in chunks) {
      for (final int byte in chunk) {
        if (byte == 0x0a) {
          final Map<String, Object?>? record = _decodeLine(line);
          line.clear();
          if (record != null) {
            yield record;
          }
          continue;
        }

        line.add(byte);
        if (line.length > maximumLineBytes) {
          throw const LichessProtocolException(
            LichessProtocolFailureKind.oversizedLine,
            'Lichess sent an oversized stream record.',
          );
        }
      }
    }

    if (line.isNotEmpty) {
      final Map<String, Object?>? record = _decodeLine(line);
      if (record != null) {
        yield record;
      }
    }
  }

  Map<String, Object?>? _decodeLine(List<int> bufferedLine) {
    final List<int> bytes = bufferedLine.isNotEmpty && bufferedLine.last == 0x0d
        ? bufferedLine.sublist(0, bufferedLine.length - 1)
        : bufferedLine;
    if (bytes.isEmpty) {
      return null;
    }

    final String source;
    try {
      source = utf8.decode(bytes);
    } on FormatException {
      throw const LichessProtocolException(
        LichessProtocolFailureKind.malformedUtf8,
        'Lichess sent a stream record that is not valid UTF-8.',
      );
    }

    final Object? decoded;
    try {
      decoded = jsonDecode(source);
    } on FormatException {
      throw const LichessProtocolException(
        LichessProtocolFailureKind.malformedJson,
        'Lichess sent a malformed JSON stream record.',
      );
    }
    if (decoded is! Map<String, dynamic>) {
      throw const LichessProtocolException(
        LichessProtocolFailureKind.nonObjectJson,
        'Lichess sent a stream record that is not a JSON object.',
      );
    }
    return Map<String, Object?>.unmodifiable(decoded);
  }
}
