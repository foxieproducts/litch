import 'dart:typed_data';

/// Carries one exact legacy or v2 Litchcraft packet through the shared Object
/// stream. Domain request ids, revisions, CRCs, and retry rules remain inside
/// the unchanged packet.
final class ConnectionLitchcraftCodec {
  const ConnectionLitchcraftCodec._();

  static const int objectOpcode = 0x40;
  static const int receiptCode = 0x40;
  static const int version = 1;
  static const int beginFlag = 1 << 0;
  static const int endFlag = 1 << 1;
  static const int _knownFlags = beginFlag | endFlag;
  static const int _beginHeaderBytes = 4;
  static const int _continuationHeaderBytes = 2;
  static const int maximumPacketBytes = 512;

  /// Returns the largest complete domain packet that fits in one Object.
  ///
  /// The first fragment carries a four-byte length/flags header. Keeping the
  /// Store packet below this boundary avoids turning one ATT-sized packet into
  /// two radio writes on a high-MTU shared session.
  static int maximumSingleObjectPacketBytes({required int maximumBodyBytes}) {
    if (maximumBodyBytes < _beginHeaderBytes + 1) {
      throw ArgumentError.value(maximumBodyBytes, 'maximumBodyBytes');
    }
    return maximumBodyBytes - _beginHeaderBytes;
  }

  static List<Uint8List> encodePacket(
    Uint8List packet, {
    required int maximumBodyBytes,
  }) {
    if (packet.isEmpty || packet.length > maximumPacketBytes) {
      throw const FormatException('Invalid shared Litchcraft packet length.');
    }
    if (maximumBodyBytes < _beginHeaderBytes + 1) {
      throw const FormatException(
        'Shared Litchcraft Object body is too small.',
      );
    }
    final fragments = <Uint8List>[];
    var offset = 0;
    var first = true;
    while (offset < packet.length) {
      final headerBytes = first ? _beginHeaderBytes : _continuationHeaderBytes;
      final available = maximumBodyBytes - headerBytes;
      if (available <= 0) {
        throw const FormatException(
          'Shared Litchcraft Object body is too small.',
        );
      }
      final count = (packet.length - offset).clamp(1, available);
      final last = offset + count == packet.length;
      final fragment = Uint8List(headerBytes + count);
      fragment[0] = version;
      fragment[1] = (first ? beginFlag : 0) | (last ? endFlag : 0);
      if (first) {
        ByteData.sublistView(
          fragment,
        ).setUint16(2, packet.length, Endian.little);
      }
      fragment.setRange(headerBytes, fragment.length, packet, offset);
      fragments.add(fragment);
      offset += count;
      first = false;
    }
    return fragments;
  }
}

final class ConnectionLitchcraftAssembler {
  final BytesBuilder _bytes = BytesBuilder(copy: false);
  int _expectedBytes = 0;

  void clear() {
    _bytes.clear();
    _expectedBytes = 0;
  }

  Uint8List? accept(Uint8List fragment) {
    try {
      if (fragment.length < 3 ||
          fragment[0] != ConnectionLitchcraftCodec.version) {
        throw const FormatException('Invalid shared Litchcraft fragment.');
      }
      final flags = fragment[1];
      if ((flags & ~ConnectionLitchcraftCodec._knownFlags) != 0) {
        throw const FormatException('Unknown shared Litchcraft flags.');
      }
      final begins = (flags & ConnectionLitchcraftCodec.beginFlag) != 0;
      final ends = (flags & ConnectionLitchcraftCodec.endFlag) != 0;
      final headerBytes = begins
          ? ConnectionLitchcraftCodec._beginHeaderBytes
          : ConnectionLitchcraftCodec._continuationHeaderBytes;
      if (fragment.length <= headerBytes || (!begins && _expectedBytes == 0)) {
        throw const FormatException(
          'Shared Litchcraft fragment is out of sequence.',
        );
      }
      if (begins) {
        // Priority Cancel can replace a response before its final fragment.
        // The reverse Object stream is delivered in sequence, so a fresh Begin
        // is an unambiguous replacement, never a continuation of old bytes.
        clear();
        final expected = ByteData.sublistView(
          fragment,
        ).getUint16(2, Endian.little);
        if (expected < 1 ||
            expected > ConnectionLitchcraftCodec.maximumPacketBytes) {
          throw const FormatException(
            'Invalid shared Litchcraft declared length.',
          );
        }
        _expectedBytes = expected;
      }
      _bytes.add(fragment.sublist(headerBytes));
      final received = _bytes.length;
      if (received > _expectedBytes || ends != (received == _expectedBytes)) {
        throw const FormatException(
          'Shared Litchcraft length disagrees with its boundary.',
        );
      }
      if (!ends) return null;
      final completed = _bytes.takeBytes();
      _expectedBytes = 0;
      return completed;
    } catch (_) {
      clear();
      rethrow;
    }
  }
}
