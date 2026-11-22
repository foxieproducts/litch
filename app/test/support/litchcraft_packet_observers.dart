import 'package:litch_mobile/src/services/litchcraft_protocol.dart';

// Discovery uses session zero. A later source-clock refresh uses the proven
// mode session; Store capability Inspect has a different payload altogether.
bool isLitchcraftModeDiscovery(List<int> packet) =>
    (packet.length == 9 || packet.length == 10 && packet[9] == 1) &&
    packet[0] == LitchcraftV2Protocol.magic &&
    packet[2] == LitchcraftV2Protocol.inspect &&
    packet.skip(3).take(4).every((byte) => byte == 0);

bool isLitchcraftSourceClockInspection(List<int> packet) =>
    packet.length == 10 &&
    packet[0] == LitchcraftV2Protocol.magic &&
    packet[2] == LitchcraftV2Protocol.inspect &&
    packet[9] == 1 &&
    packet.skip(3).take(4).any((byte) => byte != 0);
