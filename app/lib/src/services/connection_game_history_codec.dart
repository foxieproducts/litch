import 'dart:typed_data';

/// Carries one existing current-game protocol frame through the shared Object
/// stream without changing that frame's transaction, sequence, or CRC rules.
final class ConnectionGameHistoryCodec {
  const ConnectionGameHistoryCodec._();

  static const int objectOpcode = 0x30;
  static const int receiptCode = 0x30;
  static const int version = 1;
  static const int beginFlag = 1 << 0;
  static const int endFlag = 1 << 1;
  static const int _knownFlags = beginFlag | endFlag;
  static const int _beginHeaderBytes = 4;
  static const int _continuationHeaderBytes = 2;
  static const int maximumFrameBytes = 512;

  static List<Uint8List> encodeFrame(
    Uint8List frame, {
    required int maximumBodyBytes,
  }) {
    if (frame.length < 8 || frame.length > maximumFrameBytes) {
      throw const FormatException('Invalid shared game-history frame length.');
    }
    if (maximumBodyBytes < _beginHeaderBytes + 1) {
      throw const FormatException(
        'Shared game-history Object body is too small.',
      );
    }
    final List<Uint8List> fragments = <Uint8List>[];
    var offset = 0;
    var first = true;
    while (offset < frame.length) {
      final int headerBytes = first
          ? _beginHeaderBytes
          : _continuationHeaderBytes;
      final int available = maximumBodyBytes - headerBytes;
      if (available <= 0) {
        throw const FormatException(
          'Shared game-history Object body is too small.',
        );
      }
      final int count = (frame.length - offset).clamp(1, available);
      final bool last = offset + count == frame.length;
      final Uint8List fragment = Uint8List(headerBytes + count);
      fragment[0] = version;
      fragment[1] = (first ? beginFlag : 0) | (last ? endFlag : 0);
      if (first) {
        final ByteData data = ByteData.sublistView(fragment);
        data.setUint16(2, frame.length, Endian.little);
      }
      fragment.setRange(headerBytes, fragment.length, frame, offset);
      fragments.add(fragment);
      offset += count;
      first = false;
    }
    return fragments;
  }
}

final class ConnectionGameHistoryAssembler {
  final BytesBuilder _bytes = BytesBuilder(copy: false);
  int _expectedBytes = 0;

  void clear() {
    _bytes.clear();
    _expectedBytes = 0;
  }

  Uint8List? accept(Uint8List fragment) {
    try {
      if (fragment.length < 3 ||
          fragment[0] != ConnectionGameHistoryCodec.version) {
        throw const FormatException('Invalid shared game-history fragment.');
      }
      final int flags = fragment[1];
      if ((flags & ~ConnectionGameHistoryCodec._knownFlags) != 0) {
        throw const FormatException('Unknown shared game-history flags.');
      }
      final bool begins = (flags & ConnectionGameHistoryCodec.beginFlag) != 0;
      final bool ends = (flags & ConnectionGameHistoryCodec.endFlag) != 0;
      final int headerBytes = begins
          ? ConnectionGameHistoryCodec._beginHeaderBytes
          : ConnectionGameHistoryCodec._continuationHeaderBytes;
      if (fragment.length <= headerBytes || begins == (_expectedBytes != 0)) {
        throw const FormatException(
          'Shared game-history fragment is out of sequence.',
        );
      }
      if (begins) {
        final int expected = ByteData.sublistView(
          fragment,
        ).getUint16(2, Endian.little);
        if (expected < 8 ||
            expected > ConnectionGameHistoryCodec.maximumFrameBytes) {
          throw const FormatException(
            'Invalid shared game-history declared length.',
          );
        }
        _expectedBytes = expected;
      }
      _bytes.add(fragment.sublist(headerBytes));
      final int received = _bytes.length;
      if (received > _expectedBytes || ends != (received == _expectedBytes)) {
        throw const FormatException(
          'Shared game-history fragment length disagrees with its boundary.',
        );
      }
      if (!ends) return null;
      final Uint8List completed = _bytes.takeBytes();
      _expectedBytes = 0;
      return completed;
    } catch (_) {
      clear();
      rethrow;
    }
  }
}
