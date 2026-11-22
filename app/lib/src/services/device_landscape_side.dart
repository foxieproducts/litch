import 'package:flutter/services.dart';

/// The physical side of a landscape display where edge controls should sit.
enum DeviceLandscapeSide { left, right }

class DeviceOrientationSnapshot {
  const DeviceOrientationSnapshot({
    required this.side,
    this.compensateHalfTurn = false,
  });

  final DeviceLandscapeSide? side;
  final bool compensateHalfTurn;
}

class DeviceLandscapeSideReader {
  const DeviceLandscapeSideReader({MethodChannel? channel})
    : _channel = channel ?? const MethodChannel(_channelName);

  static const String _channelName =
      'com.foxieproducts.litch/interface_orientation';

  final MethodChannel _channel;

  /// The native scene reports whether Flutter must compensate for an actual
  /// interface lock. Unsupported hosts retain their normal interface rotation.
  Future<DeviceOrientationSnapshot> readSnapshot() async {
    try {
      final value = await _channel.invokeMapMethod<String, Object?>(
        'orientationState',
      );
      return DeviceOrientationSnapshot(
        side: switch (value?['side']) {
          'left' => DeviceLandscapeSide.left,
          'right' => DeviceLandscapeSide.right,
          _ => null,
        },
        compensateHalfTurn: value?['compensateHalfTurn'] == true,
      );
    } on PlatformException {
      return DeviceOrientationSnapshot(side: await read());
    } on MissingPluginException {
      return DeviceOrientationSnapshot(side: await read());
    }
  }

  /// Reads the current native interface orientation once.
  ///
  /// Portrait, face-up, unsupported, and temporarily unavailable states return
  /// null. Callers retain their last known landscape side in those cases.
  Future<DeviceLandscapeSide?> read() async {
    try {
      return switch (await _channel.invokeMethod<String>('landscapeSide')) {
        'left' => DeviceLandscapeSide.left,
        'right' => DeviceLandscapeSide.right,
        _ => null,
      };
    } on PlatformException {
      return null;
    } on MissingPluginException {
      return null;
    }
  }
}
