import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/board_switch_state.dart';

void main() {
  test('parses every fitted independently keyed switch state', () {
    for (var index = 0; index < BoardSwitchState.switchCount; index += 1) {
      final switchId = BoardSwitch.values[index];
      final button = switch (switchId) {
        BoardSwitch.p1Up || BoardSwitch.p2Up => 0,
        BoardSwitch.p1Down || BoardSwitch.p2Down => 1,
        BoardSwitch.p1Left || BoardSwitch.p2Left => 2,
        BoardSwitch.p1Right || BoardSwitch.p2Right => 3,
        BoardSwitch.p1Center || BoardSwitch.p2Center => 4,
        BoardSwitch.p1X => 5,
        BoardSwitch.p2X => 6,
        BoardSwitch.p1O => 7,
        BoardSwitch.p2O => 8,
      };
      final owner = index < 5 || index == 10 || index == 11 ? 1 : 2;
      final body = Uint8List(8)
        ..[0] = BoardSwitchState.version
        ..[1] = button
        ..[2] = owner
        ..[3] = 1;
      ByteData.sublistView(body).setUint32(4, 0x12345678, Endian.little);

      final state = BoardSwitchState.tryParse(
        code: BoardSwitchState.firstStateCode + index,
        revision: index + 1,
        body: body,
      );

      if (index == 4 || index == 9) {
        expect(state, isNull, reason: 'Reserved input cannot become a control');
        continue;
      }
      expect(state?.switchId, switchId);
      expect(state?.isDown, isTrue);
      expect(state?.revision, index + 1);
      expect(state?.observedAtMs, 0x12345678);
    }
  });

  test('rejects malformed or mismatched switch identity', () {
    final valid = Uint8List.fromList(<int>[1, 0, 1, 0, 0, 0, 0, 0]);
    expect(
      BoardSwitchState.tryParse(code: 23, revision: 1, body: valid),
      isNotNull,
    );
    expect(
      BoardSwitchState.tryParse(code: 24, revision: 2, body: valid),
      isNull,
    );
    expect(
      BoardSwitchState.tryParse(
        code: 23,
        revision: 2,
        body: Uint8List.fromList(<int>[2, 0, 1, 0, 0, 0, 0, 0]),
      ),
      isNull,
    );
    expect(
      BoardSwitchState.tryParse(
        code: 23,
        revision: 2,
        body: Uint8List.fromList(<int>[1, 0, 1, 2, 0, 0, 0, 0]),
      ),
      isNull,
    );
  });
}
