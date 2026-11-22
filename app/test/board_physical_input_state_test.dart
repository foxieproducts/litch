import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/board_physical_input_state.dart';
import 'package:litch_mobile/src/models/board_switch_state.dart';

void main() {
  test('one minimum-payload body carries every sampled physical input', () {
    final body = Uint8List(BoardPhysicalInputState.bodyBytes);
    ByteData.sublistView(body)
      ..setUint64(0, 0x8000000000000001, Endian.little)
      ..setUint16(8, (1 << 3) | (1 << 11), Endian.little);

    final state = BoardPhysicalInputState.tryParse(
      code: BoardPhysicalInputState.stateCode,
      revision: 17,
      body: body,
    );

    expect(state, isNotNull);
    expect(state!.occupancyMask, 0x8000000000000001);
    expect(state.isDown(BoardSwitch.p1Right), isTrue);
    expect(state.isDown(BoardSwitch.p2O), isTrue);
    expect(state.isDown(BoardSwitch.p1Center), isFalse);
    expect(state.isDown(BoardSwitch.p2Center), isFalse);
  });

  test('rejects reserved switch bits and legacy per-switch bodies', () {
    final reserved = Uint8List(BoardPhysicalInputState.bodyBytes);
    ByteData.sublistView(reserved).setUint16(8, 1 << 12, Endian.little);
    expect(
      BoardPhysicalInputState.tryParse(
        code: BoardPhysicalInputState.stateCode,
        revision: 1,
        body: reserved,
      ),
      isNull,
    );
    expect(
      BoardPhysicalInputState.tryParse(
        code: BoardPhysicalInputState.stateCode,
        revision: 1,
        body: Uint8List(8),
      ),
      isNull,
    );
  });
}
