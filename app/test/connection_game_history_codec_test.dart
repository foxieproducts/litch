import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/services/connection_game_history_codec.dart';

void main() {
  test('minimum session body fragments and reconstructs one exact frame', () {
    final Uint8List frame = Uint8List.fromList(
      List<int>.generate(20, (int index) => index + 1),
    );
    final List<Uint8List> fragments = ConnectionGameHistoryCodec.encodeFrame(
      frame,
      maximumBodyBytes: 10,
    );

    expect(fragments.map((Uint8List value) => value.length), <int>[10, 10, 8]);
    expect(fragments.first.sublist(0, 4), <int>[1, 1, 20, 0]);
    expect(fragments[1].sublist(0, 2), <int>[1, 0]);
    expect(fragments.last.sublist(0, 2), <int>[1, 2]);

    final ConnectionGameHistoryAssembler assembler =
        ConnectionGameHistoryAssembler();
    expect(assembler.accept(fragments[0]), isNull);
    expect(assembler.accept(fragments[1]), isNull);
    expect(assembler.accept(fragments[2]), frame);
  });

  test('large session body keeps a command frame in one object', () {
    final Uint8List frame = Uint8List.fromList(List<int>.filled(20, 0x5a));
    final List<Uint8List> fragments = ConnectionGameHistoryCodec.encodeFrame(
      frame,
      maximumBodyBytes: 234,
    );

    expect(fragments, hasLength(1));
    expect(fragments.single.sublist(0, 4), <int>[1, 3, 20, 0]);
    expect(ConnectionGameHistoryAssembler().accept(fragments.single), frame);
  });

  test('assembler discards partial state after malformed boundary', () {
    final ConnectionGameHistoryAssembler assembler =
        ConnectionGameHistoryAssembler();
    expect(
      assembler.accept(Uint8List.fromList(<int>[1, 1, 8, 0, 1, 2, 3])),
      isNull,
    );
    expect(
      () => assembler.accept(Uint8List.fromList(<int>[1, 2, 4])),
      throwsFormatException,
    );

    final Uint8List complete = Uint8List.fromList(<int>[
      1,
      3,
      8,
      0,
      1,
      2,
      3,
      4,
      5,
      6,
      7,
      8,
    ]);
    expect(assembler.accept(complete), <int>[1, 2, 3, 4, 5, 6, 7, 8]);
  });

  test('encoder rejects impossible bounds', () {
    expect(
      () => ConnectionGameHistoryCodec.encodeFrame(
        Uint8List(7),
        maximumBodyBytes: 10,
      ),
      throwsFormatException,
    );
    expect(
      () => ConnectionGameHistoryCodec.encodeFrame(
        Uint8List(20),
        maximumBodyBytes: 4,
      ),
      throwsFormatException,
    );
  });
}
