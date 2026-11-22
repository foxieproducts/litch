import 'dart:typed_data';

/// Identify uses the common Ordered receipt; Probe is a no-effect support check.
abstract final class ConnectionIdentifyCodec {
  static const opcode = 28;
  static Uint8List probe() => Uint8List.fromList([1, 0, 0, 0, 0, 0, 0, 0]);
  static Uint8List stop() => Uint8List.fromList([1, 2, 0, 0, 0, 0, 0, 0]);
  static Uint8List start(int number, int durationMs) {
    if (number < 0 || number > 99 || durationMs < 250 || durationMs > 60000) {
      throw const FormatException('Invalid Identify request.');
    }
    return Uint8List.fromList([
      1,
      1,
      number,
      0,
      0,
      255,
      durationMs & 255,
      durationMs >> 8,
    ]);
  }
}
