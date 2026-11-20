import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/services/connection_ota_status_codec.dart';

void main() {
  test('minimum session body reconstructs exact OTA status JSON', () {
    final Uint8List json = Uint8List.fromList(
      '{"status":"progress","percent":42}'.codeUnits,
    );
    final ConnectionOtaStatusAssembler assembler =
        ConnectionOtaStatusAssembler();
    final Uint8List first = Uint8List.fromList(<int>[
      1,
      1,
      json.length,
      0,
      ...json.sublist(0, 6),
    ]);
    expect(assembler.accept(first), isNull);
    var offset = 6;
    while (json.length - offset > 8) {
      expect(
        assembler.accept(
          Uint8List.fromList(<int>[1, 0, ...json.sublist(offset, offset + 8)]),
        ),
        isNull,
      );
      offset += 8;
    }
    expect(
      assembler.accept(
        Uint8List.fromList(<int>[1, 2, ...json.sublist(offset)]),
      ),
      json,
    );
  });

  test('new begin replaces an obsolete partial progress status', () {
    final ConnectionOtaStatusAssembler assembler =
        ConnectionOtaStatusAssembler();
    expect(
      assembler.accept(Uint8List.fromList(<int>[1, 1, 8, 0, 1, 2, 3])),
      isNull,
    );
    expect(
      assembler.accept(Uint8List.fromList(<int>[1, 3, 5, 0, 9, 8, 7, 6, 5])),
      <int>[9, 8, 7, 6, 5],
    );
  });

  test('malformed continuation clears partial assembly', () {
    final ConnectionOtaStatusAssembler assembler =
        ConnectionOtaStatusAssembler();
    expect(
      assembler.accept(Uint8List.fromList(<int>[1, 1, 5, 0, 1, 2])),
      isNull,
    );
    expect(
      () => assembler.accept(Uint8List.fromList(<int>[1, 2, 3])),
      throwsFormatException,
    );
    expect(
      () => assembler.accept(Uint8List.fromList(<int>[1, 2, 4])),
      throwsFormatException,
    );
  });
}
