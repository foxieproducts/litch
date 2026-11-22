import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/services/device_landscape_side.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  const channel = MethodChannel(
    'com.foxieproducts.litch/interface_orientation',
  );

  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, null);
  });

  test(
    'maps both native landscape directions and preserves unknown states',
    () async {
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      var response = 'left';
      messenger.setMockMethodCallHandler(channel, (call) async {
        expect(call.method, 'landscapeSide');
        return response;
      });
      const reader = DeviceLandscapeSideReader(channel: channel);

      expect(await reader.read(), DeviceLandscapeSide.left);
      response = 'right';
      expect(await reader.read(), DeviceLandscapeSide.right);
      response = 'portrait';
      expect(await reader.read(), isNull);
    },
  );

  test('fails soft when the native bridge is unavailable', () async {
    const reader = DeviceLandscapeSideReader(channel: channel);
    expect(await reader.read(), isNull);
  });

  test(
    'snapshot uses actual native lock compensation and falls back on old hosts',
    () async {
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      var modern = true;
      messenger.setMockMethodCallHandler(channel, (call) async {
        if (call.method == 'orientationState') {
          if (!modern) throw MissingPluginException();
          return {'side': 'left', 'compensateHalfTurn': true};
        }
        return 'right';
      });
      const reader = DeviceLandscapeSideReader(channel: channel);
      final locked = await reader.readSnapshot();
      expect(locked.side, DeviceLandscapeSide.left);
      expect(locked.compensateHalfTurn, isTrue);
      modern = false;
      final fallback = await reader.readSnapshot();
      expect(fallback.side, DeviceLandscapeSide.right);
      expect(fallback.compensateHalfTurn, isFalse);
    },
  );
}
