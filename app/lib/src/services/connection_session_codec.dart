import 'dart:typed_data';

enum ConnectionSessionKind { control, state, patch, ordered, object }

abstract final class ConnectionSessionControl {
  static const hello = 1;
  static const welcome = 2;
  static const identity = 3;
  static const ready = 4;
  static const activated = 5;
  static const open = 6;
  static const opened = 7;
  static const progress = 8;
  static const checkpoint = 9;
  static const outcome = 10;
  static const confirm = 11;
  static const query = 12;
  static const close = 13;
  static const closed = 14;
  static const heartbeat = 15;
}

/// The decoded body borrows the source packet. The session must own/copy bytes
/// before retaining them across native callbacks; framing does not admit work.
final class ConnectionSessionPacket {
  const ConnectionSessionPacket({
    required this.kind,
    required this.code,
    required this.token,
    required this.sequence,
    required this.body,
  });

  final ConnectionSessionKind kind;
  final int code;
  final int token;
  final int sequence;
  final Uint8List body;
}

/// Version-four framing shared with the firmware codec and byte fixtures.
/// Activation, negotiated stream bindings and domain bodies are session-owned.
abstract final class ConnectionSessionCodec {
  static const version = 4;
  static const headerBytes = 10;
  static const minimumPacketBytes = 20;
  static const maximumPacketBytes = 512;
  static const lastSequence = 0xfffffffe;
  // Wire bounds; each opened stream retains its own smaller selected grant.
  static const maximumWindow = 64;
  static const maximumOrderedCapacity = 16;
  static const maximumReverseCapacity = 64;
  static const maximumExpectedFeedbackMs = 60000;
  static const knownFeaturesMask = 0x007f;

  static ConnectionSessionPacket? decode(
    Uint8List bytes, {
    int packetLimit = maximumPacketBytes,
  }) {
    if (!_validLimit(packetLimit) ||
        bytes.length < headerBytes ||
        bytes.length > packetLimit ||
        bytes[0] >> 4 != version ||
        (bytes[0] & 15) >= ConnectionSessionKind.values.length) {
      return null;
    }
    final data = ByteData.sublistView(bytes);
    final packet = ConnectionSessionPacket(
      kind: ConnectionSessionKind.values[bytes[0] & 15],
      code: bytes[1],
      token: data.getUint32(2, Endian.little),
      sequence: data.getUint32(6, Endian.little),
      body: Uint8List.sublistView(bytes, headerBytes),
    );
    return _valid(packet) ? packet : null;
  }

  static Uint8List encode(ConnectionSessionPacket packet) {
    if (packet.body.length > maximumPacketBytes - headerBytes ||
        !_valid(packet)) {
      throw const FormatException('Invalid connection-session packet');
    }
    final bytes = Uint8List(headerBytes + packet.body.length);
    final data = ByteData.sublistView(bytes);
    bytes[0] = (version << 4) | packet.kind.index;
    bytes[1] = packet.code;
    data.setUint32(2, packet.token, Endian.little);
    data.setUint32(6, packet.sequence, Endian.little);
    bytes.setRange(headerBytes, bytes.length, packet.body);
    return bytes;
  }

  static bool _validLimit(int value) =>
      value >= minimumPacketBytes && value <= maximumPacketBytes;

  static bool _validGrant(int bodyBytes, int capacity, int lane) =>
      capacity >= 1 &&
      capacity <=
          (lane == 0 ? maximumOrderedCapacity : maximumReverseCapacity) &&
      lane <= 1 &&
      bodyBytes >= 1 &&
      bodyBytes <= maximumPacketBytes - headerBytes &&
      (lane != 0 || bodyBytes <= minimumPacketBytes - headerBytes);

  static int _lowBits(int count) => count >= 16 ? 0xffff : (1 << count) - 1;

  static bool _valid(ConnectionSessionPacket packet) {
    if (packet.code < 0 ||
        packet.code > 255 ||
        packet.token < 0 ||
        packet.token > 0xffffffff ||
        packet.sequence < 0 ||
        packet.sequence > lastSequence) {
      return false;
    }
    if (packet.kind != ConnectionSessionKind.control) {
      return packet.token != 0 && packet.sequence != 0;
    }
    if ((packet.token == 0) !=
        (packet.code == ConnectionSessionControl.hello)) {
      return false;
    }
    final body = packet.body;
    final data = ByteData.sublistView(body);
    int u16(int offset) => data.getUint16(offset, Endian.little);
    int u32(int offset) => data.getUint32(offset, Endian.little);
    bool noncePresent() => body.take(8).any((value) => value != 0);
    switch (packet.code) {
      case ConnectionSessionControl.hello:
      case ConnectionSessionControl.welcome:
        return packet.sequence == 0 &&
            body.length == 10 &&
            noncePresent() &&
            _validLimit(u16(8));
      case ConnectionSessionControl.identity:
        return packet.sequence == 0 &&
            body.length == 10 &&
            noncePresent() &&
            u16(8) & ~knownFeaturesMask == 0;
      case ConnectionSessionControl.ready:
      case ConnectionSessionControl.activated:
        return packet.sequence == 0 && body.length == 4;
      case ConnectionSessionControl.open:
        return packet.sequence != 0 &&
            body.length == 8 &&
            u16(0) != 0 &&
            _validGrant(u16(4), body[3], body[2]) &&
            u16(6) <= maximumExpectedFeedbackMs;
      case ConnectionSessionControl.opened:
        if (packet.sequence == 0 || body.length != 10 || body[7] > 1) {
          return false;
        }
        return u16(8) == 0
            ? u32(0) != 0 &&
                  u32(0) != packet.token &&
                  _validGrant(u16(4), body[6], body[7])
            : u32(0) == 0 && u16(4) == 0 && body[6] == 0;
      case ConnectionSessionControl.progress:
        if (body.length != 10) return false;
        final confirmed = u32(0);
        final granted = u32(4);
        return confirmed <= packet.sequence &&
            packet.sequence <= granted &&
            granted <= lastSequence &&
            granted - confirmed <= maximumWindow &&
            u16(8) & ~_lowBits(granted - packet.sequence) == 0;
      case ConnectionSessionControl.checkpoint:
        return body.length == 10 && u16(8) & ~_lowBits(packet.sequence) == 0;
      case ConnectionSessionControl.outcome:
        return packet.sequence != 0 &&
            body.length == 3 &&
            body[0] <= 2 &&
            ((body[0] == 0) == (u16(1) == 0));
      case ConnectionSessionControl.confirm:
      case ConnectionSessionControl.query:
        return body.isEmpty;
      case ConnectionSessionControl.close:
      case ConnectionSessionControl.closed:
      case ConnectionSessionControl.heartbeat:
        return packet.sequence == 0 && body.isEmpty;
      default:
        return false;
    }
  }
}
