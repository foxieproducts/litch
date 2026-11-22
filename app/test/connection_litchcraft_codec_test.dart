import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/services/connection_litchcraft_codec.dart';

void main() {
  test('high-MTU Store packets fit in one Object at the fragment boundary', () {
    expect(
      ConnectionLitchcraftCodec.maximumSingleObjectPacketBytes(
        maximumBodyBytes: 234,
      ),
      230,
    );
    expect(
      ConnectionLitchcraftCodec.encodePacket(
        Uint8List(230),
        maximumBodyBytes: 234,
      ),
      hasLength(1),
    );
    expect(
      ConnectionLitchcraftCodec.encodePacket(
        Uint8List(231),
        maximumBodyBytes: 234,
      ),
      hasLength(2),
    );
  });

  test('minimum Object body reconstructs one exact Litchcraft packet', () {
    final packet = Uint8List.fromList(
      List<int>.generate(244, (index) => index & 0xff),
    );
    final fragments = ConnectionLitchcraftCodec.encodePacket(
      packet,
      maximumBodyBytes: 10,
    );
    final assembler = ConnectionLitchcraftAssembler();
    Uint8List? completed;
    for (final fragment in fragments) {
      expect(fragment.length, lessThanOrEqualTo(10));
      completed = assembler.accept(fragment) ?? completed;
    }
    expect(completed, orderedEquals(packet));
    expect(fragments.length, greaterThan(16));
  });

  test('malformed fragment clears staging for a clean retry', () {
    final packet = Uint8List.fromList(List<int>.generate(20, (i) => i + 1));
    final fragments = ConnectionLitchcraftCodec.encodePacket(
      packet,
      maximumBodyBytes: 10,
    );
    final assembler = ConnectionLitchcraftAssembler();
    expect(assembler.accept(fragments.first), isNull);
    expect(
      () => assembler.accept(Uint8List.fromList(<int>[1, 2, 99])),
      throwsFormatException,
    );
    Uint8List? completed;
    for (final fragment in fragments) {
      completed = assembler.accept(fragment) ?? completed;
    }
    expect(completed, orderedEquals(packet));
  });

  test('a new Begin abandons an interrupted exact packet', () {
    final first = ConnectionLitchcraftCodec.encodePacket(
      Uint8List.fromList(List<int>.filled(20, 1)),
      maximumBodyBytes: 10,
    );
    final second = ConnectionLitchcraftCodec.encodePacket(
      Uint8List.fromList(List<int>.filled(20, 2)),
      maximumBodyBytes: 10,
    );
    final assembler = ConnectionLitchcraftAssembler();
    expect(assembler.accept(first.first), isNull);
    Uint8List? completed;
    for (final fragment in second) {
      completed = assembler.accept(fragment) ?? completed;
    }
    expect(completed, orderedEquals(List<int>.filled(20, 2)));
  });
}
