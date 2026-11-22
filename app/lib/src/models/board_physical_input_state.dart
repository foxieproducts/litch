import 'dart:typed_data';

import 'board_switch_state.dart';

/// The current physical inputs sampled by V2/V3 in one retained BLE packet.
final class BoardPhysicalInputState {
  const BoardPhysicalInputState({
    required this.occupancyMask,
    required this.switchMask,
    required this.revision,
  });

  static const int stateCode = 23;
  static const int bodyBytes = 10;
  static const int switchCount = 12;
  static const int validSwitchMask = (1 << switchCount) - 1;
  static const List<BoardSwitch> sampledSwitches = <BoardSwitch>[
    BoardSwitch.p1Up,
    BoardSwitch.p1Down,
    BoardSwitch.p1Left,
    BoardSwitch.p1Right,
    BoardSwitch.p2Up,
    BoardSwitch.p2Down,
    BoardSwitch.p2Left,
    BoardSwitch.p2Right,
    BoardSwitch.p1X,
    BoardSwitch.p1O,
    BoardSwitch.p2X,
    BoardSwitch.p2O,
  ];

  final int occupancyMask;
  final int switchMask;
  final int revision;

  bool isDown(BoardSwitch switchId) {
    final int index = sampledSwitches.indexOf(switchId);
    return index >= 0 && (switchMask & (1 << index)) != 0;
  }

  static BoardPhysicalInputState? tryParse({
    required int code,
    required int revision,
    required Uint8List body,
  }) {
    if (code != stateCode || revision <= 0 || body.length != bodyBytes) {
      return null;
    }
    final ByteData data = ByteData.sublistView(body);
    final int switches = data.getUint16(8, Endian.little);
    if ((switches & ~validSwitchMask) != 0) return null;
    return BoardPhysicalInputState(
      occupancyMask: data.getUint64(0, Endian.little),
      switchMask: switches,
      revision: revision,
    );
  }
}
