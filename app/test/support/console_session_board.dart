import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/litch_device_capabilities.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/litch_control_protocol.dart';

import 'reboot_reply_ble.dart';

// External board receiver for the real Ordered Console path. Completed commands
// are exposed after every fragment has reached the board.
class ConsoleSessionBoard extends RebootReplyBle {
  ConsoleSessionBoard(this.commands);
  final List<String> commands;
  final List<int> _record = <int>[];

  @override
  int orderedRejection(ConnectionSessionPacket packet) {
    if (packet.code != LitchControlProtocol.connectionConsoleOpcode) return 4;
    final body = packet.body;
    if (body.length < 2 || body[0] != 1) return 4;
    final begin = body[1] & LitchControlProtocol.connectionConsoleBegin != 0;
    final end = body[1] & LitchControlProtocol.connectionConsoleEnd != 0;
    if (begin) _record.clear();
    _record.addAll(body.sublist(begin ? 3 : 2));
    if (end) {
      commands.add(utf8.decode(_record));
      _record.clear();
    }
    return 0;
  }
}

Future<void> openConsoleTestSession(
  WidgetTester tester,
  LitchBleService service,
  String deviceId,
) async {
  if (service.isConnected) {
    await service.disconnect();
  }
  service.debugSetConnectedDevice(deviceId);
  service.debugSetDeviceCapabilities(
    deviceId,
    const LitchDeviceCapabilities(
      contractVersion: 3,
      hardwareRevision: LitchHardwareRevision.v3,
      capabilityMask: LitchDeviceCapabilities.consoleBit,
      explicitlyReported: true,
    ),
  );
  final opening = service.openConnectionSession(deviceId);
  await tester.pump();
  expect((await opening)?.isActivated, isTrue);
}
