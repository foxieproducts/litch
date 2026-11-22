import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/online/lichess/lichess_ndjson_decoder.dart';
import 'package:litch_mobile/src/online/lichess/lichess_protocol.dart';

void main() {
  test('joins fragments and splits coalesced LF and CRLF records', () async {
    const LichessNdjsonDecoder decoder = LichessNdjsonDecoder();
    final Stream<List<int>> chunks = Stream<List<int>>.fromIterable(<List<int>>[
      utf8.encode('{"type":"game'),
      utf8.encode('Full","extra":1}\n\n{"type":"gameState"}\r'),
      utf8.encode('\n'),
    ]);

    expect(await decoder.decode(chunks).toList(), <Map<String, Object?>>[
      <String, Object?>{'type': 'gameFull', 'extra': 1},
      <String, Object?>{'type': 'gameState'},
    ]);
  });

  test('accepts a final record without a newline', () async {
    const LichessNdjsonDecoder decoder = LichessNdjsonDecoder();

    expect(
      await decoder
          .decode(Stream<List<int>>.value(utf8.encode('{"type":"done"}')))
          .single,
      <String, Object?>{'type': 'done'},
    );
  });

  test('rejects an oversized buffered line without echoing it', () async {
    const LichessNdjsonDecoder decoder = LichessNdjsonDecoder(
      maximumLineBytes: 8,
    );

    await expectLater(
      decoder
          .decode(Stream<List<int>>.value(utf8.encode('{"secret":"x"}\n')))
          .drain<void>(),
      throwsA(
        isA<LichessProtocolException>()
            .having(
              (LichessProtocolException error) => error.kind,
              'kind',
              LichessProtocolFailureKind.oversizedLine,
            )
            .having(
              (LichessProtocolException error) => error.toString(),
              'redacted message',
              isNot(contains('secret')),
            ),
      ),
    );
  });

  test('rejects malformed UTF-8, JSON, and non-object JSON', () async {
    const LichessNdjsonDecoder decoder = LichessNdjsonDecoder();

    for (final ({List<int> bytes, LichessProtocolFailureKind kind}) fixture
        in <({List<int> bytes, LichessProtocolFailureKind kind})>[
          (
            bytes: <int>[0xc3, 0x28, 0x0a],
            kind: LichessProtocolFailureKind.malformedUtf8,
          ),
          (
            bytes: utf8.encode('{broken}\n'),
            kind: LichessProtocolFailureKind.malformedJson,
          ),
          (
            bytes: utf8.encode('[1,2,3]\n'),
            kind: LichessProtocolFailureKind.nonObjectJson,
          ),
        ]) {
      await expectLater(
        decoder.decode(Stream<List<int>>.value(fixture.bytes)).drain<void>(),
        throwsA(
          isA<LichessProtocolException>().having(
            (LichessProtocolException error) => error.kind,
            'kind',
            fixture.kind,
          ),
        ),
      );
    }
  });
}
