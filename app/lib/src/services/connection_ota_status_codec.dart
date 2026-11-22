import 'dart:typed_data';

/// Reassembles exact OTA status JSON carried by shared Object receipts.
///
/// Status is retained-latest data: a new begin fragment intentionally replaces
/// an incomplete older status (normally an obsolete progress update).
final class ConnectionOtaStatusAssembler {
  static const int version = 1;
  static const int beginFlag = 1 << 0;
  static const int endFlag = 1 << 1;
  static const int _knownFlags = beginFlag | endFlag;
  static const int _beginHeaderBytes = 4;
  static const int _continuationHeaderBytes = 2;
  static const int maximumStatusBytes = 224;

  final BytesBuilder _bytes = BytesBuilder(copy: false);
  int _expectedBytes = 0;

  void clear() {
    _bytes.clear();
    _expectedBytes = 0;
  }

  Uint8List? accept(Uint8List fragment) {
    try {
      if (fragment.length < 3 || fragment[0] != version) {
        throw const FormatException('Invalid shared OTA status fragment.');
      }
      final int flags = fragment[1];
      if ((flags & ~_knownFlags) != 0) {
        throw const FormatException('Unknown shared OTA status flags.');
      }
      final bool begins = (flags & beginFlag) != 0;
      final bool ends = (flags & endFlag) != 0;
      final int headerBytes = begins
          ? _beginHeaderBytes
          : _continuationHeaderBytes;
      if (fragment.length <= headerBytes || (!begins && _expectedBytes == 0)) {
        throw const FormatException(
          'Shared OTA status fragment is out of sequence.',
        );
      }
      if (begins) {
        clear();
        final int expected = ByteData.sublistView(
          fragment,
        ).getUint16(2, Endian.little);
        if (expected < 1 || expected > maximumStatusBytes) {
          throw const FormatException(
            'Invalid shared OTA status declared length.',
          );
        }
        _expectedBytes = expected;
      }
      _bytes.add(fragment.sublist(headerBytes));
      final int received = _bytes.length;
      if (received > _expectedBytes || ends != (received == _expectedBytes)) {
        throw const FormatException(
          'Shared OTA status length disagrees with its boundary.',
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
