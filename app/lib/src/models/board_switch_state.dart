import 'dart:typed_data';

enum BoardSwitch {
  p1Up,
  p1Down,
  p1Left,
  p1Right,
  p1Center, // Reserved legacy position; never a sampled control.
  p2Up,
  p2Down,
  p2Left,
  p2Right,
  p2Center, // Reserved legacy position; never a sampled control.
  p1X,
  p1O,
  p2X,
  p2O,
}

final class BoardSwitchState {
  const BoardSwitchState({
    required this.switchId,
    required this.isDown,
    required this.revision,
    required this.observedAtMs,
  });

  static const int version = 1;
  static const int firstStateCode = 23;
  static const int switchCount = 14;
  static const int lastStateCode = firstStateCode + switchCount - 1;
  static const int bodyBytes = 8;

  final BoardSwitch switchId;
  final bool isDown;
  final int revision;
  final int observedAtMs;

  static BoardSwitchState? tryParse({
    required int code,
    required int revision,
    required Uint8List body,
  }) {
    if (code < firstStateCode ||
        code > lastStateCode ||
        body.length != bodyBytes ||
        body[0] != version ||
        body[3] > 1) {
      return null;
    }
    final int index = code - firstStateCode;
    if (index == 4 || index == 9) return null;
    final BoardSwitch switchId = BoardSwitch.values[index];
    final int expectedButton = switch (switchId) {
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
    final int expectedOwner = index < 5 || index == 10 || index == 11 ? 1 : 2;
    if (body[1] != expectedButton || body[2] != expectedOwner) return null;
    return BoardSwitchState(
      switchId: switchId,
      isDown: body[3] == 1,
      revision: revision,
      observedAtMs: ByteData.sublistView(body).getUint32(4, Endian.little),
    );
  }
}
