import 'dart:typed_data';

/// Shared-session adapter identifiers for the existing verified OTA owner.
final class ConnectionOtaCodec {
  const ConnectionOtaCodec._();

  static const int dataObjectOpcode = 0x31;
  static const int controlObjectOpcode = 0x32;
  static const int version = 1;
  static const int beginFlag = 1 << 0;
  static const int endFlag = 1 << 1;
  static const int _beginHeaderBytes = 4;
  static const int _continuationHeaderBytes = 2;
  static const int maximumControlBytes = 191;

  static List<Uint8List> encodeControl(
    Uint8List control, {
    required int maximumBodyBytes,
  }) {
    if (control.isEmpty || control.length > maximumControlBytes) {
      throw const FormatException('Invalid shared OTA control length.');
    }
    if (maximumBodyBytes < _beginHeaderBytes + 1) {
      throw const FormatException('Shared OTA Object body is too small.');
    }
    final List<Uint8List> fragments = <Uint8List>[];
    var offset = 0;
    var first = true;
    while (offset < control.length) {
      final int headerBytes = first
          ? _beginHeaderBytes
          : _continuationHeaderBytes;
      final int available = maximumBodyBytes - headerBytes;
      if (available <= 0) {
        throw const FormatException('Shared OTA Object body is too small.');
      }
      final int count = (control.length - offset).clamp(1, available);
      final bool last = offset + count == control.length;
      final Uint8List fragment = Uint8List(headerBytes + count);
      fragment[0] = version;
      fragment[1] = (first ? beginFlag : 0) | (last ? endFlag : 0);
      if (first) {
        ByteData.sublistView(
          fragment,
        ).setUint16(2, control.length, Endian.little);
      }
      fragment.setRange(headerBytes, fragment.length, control, offset);
      fragments.add(fragment);
      offset += count;
      first = false;
    }
    return fragments;
  }
}
