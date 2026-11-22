import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/services/connection_ota_codec.dart';

void main() {
  test('minimum session body fragments one exact OTA control', () {
    final Uint8List control = Uint8List.fromList(
      List<int>.generate(19, (int index) => index + 1),
    );
    final fragments = ConnectionOtaCodec.encodeControl(
      control,
      maximumBodyBytes: 10,
    );

    expect(fragments.map((value) => value.length), <int>[10, 10, 7]);
    expect(fragments.first.sublist(0, 4), <int>[1, 1, 19, 0]);
    expect(fragments[1].sublist(0, 2), <int>[1, 0]);
    expect(fragments.last.sublist(0, 2), <int>[1, 2]);
    expect(<int>[
      ...fragments.first.sublist(4),
      ...fragments[1].sublist(2),
      ...fragments.last.sublist(2),
    ], control);
  });

  test('large session body keeps one OTA control in one object', () {
    final Uint8List control = Uint8List.fromList(List<int>.filled(50, 0x5a));
    final fragments = ConnectionOtaCodec.encodeControl(
      control,
      maximumBodyBytes: 234,
    );

    expect(fragments, hasLength(1));
    expect(fragments.single.sublist(0, 4), <int>[1, 3, 50, 0]);
    expect(fragments.single.sublist(4), control);
  });

  test('encoder rejects impossible control and body bounds', () {
    expect(
      () =>
          ConnectionOtaCodec.encodeControl(Uint8List(0), maximumBodyBytes: 10),
      throwsFormatException,
    );
    expect(
      () => ConnectionOtaCodec.encodeControl(
        Uint8List(ConnectionOtaCodec.maximumControlBytes + 1),
        maximumBodyBytes: 234,
      ),
      throwsFormatException,
    );
    expect(
      () => ConnectionOtaCodec.encodeControl(Uint8List(1), maximumBodyBytes: 4),
      throwsFormatException,
    );
  });
}
